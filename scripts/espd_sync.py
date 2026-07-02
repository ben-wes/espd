#!/usr/bin/env python3
"""
Watch a local Pure Data project folder and sync to ESP local storage over CDC or WiFi.

Requires: pip install pyserial (serial only; WiFi uses stdlib socket)

Usage:
  python3 scripts/espd_sync.py [-p PORT] [./my_patch]
  python3 scripts/espd_sync.py --host 192.168.4.1 [./my_patch]

PORT is the OTG CDC serial device (pyserial): e.g. /dev/cu.usbmodem* (macOS),
/dev/ttyACM0 (Linux), COM3 (Windows). See docs/DEV_SYNC.md.

Join the device SoftAP first for WiFi sync (default TCP port 4499).

The ESP must have CONFIG_ESPD_DEV_CDC_SYNC or CONFIG_ESPD_WIFI_AP_SYNC for dev sync.
Do not run idf.py monitor on the same serial port at the same time.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import sys
import zlib
import threading
import time
from dataclasses import dataclass

_PUT_ACK_RE = re.compile(rb"^\+OK PUT ack (\d+)$")
_PUT_READY_RE = re.compile(rb"^\+OK PUT ready window=(\d+)$")
_SERIAL_BAUD = 921600


def _put_window_from_ready(line: bytes) -> int:
    m = _PUT_READY_RE.match(line.strip())
    if not m:
        raise RuntimeError(f"unexpected PUT reply: {line.decode(errors='replace')}")
    w = int(m.group(1), 10)
    if w <= 0:
        raise RuntimeError(f"invalid PUT window: {w}")
    return w


@dataclass
class OutputConfig:
    color: bool = True
    show_esp: bool = True


_out = OutputConfig()
_RESET = "\033[0m"
_STYLES = {
    "dev-ok": "\033[32m",
    "dev-err": "\033[31m",
    "dev-tx": "\033[36m",
    "espd": "\033[1;36m",
    "pd": "\033[97m",
    "esp-i": "\033[90m",
    "esp-d": "\033[90m",
    "esp-v": "\033[90m",
    "esp-w": "\033[93m",
    "esp-e": "\033[31m",
    "script": "\033[2m",
}

_ANSI_RE = re.compile(rb"\x1b\[[0-9;]*m")
_ESP_LOG_RE = re.compile(rb"^[IWEDV] \([^)]+\) [^:\n]+: ")
_PUT_DONE_RE = re.compile(rb"^\+OK PUT done ([0-9a-fA-F]{8})$")
_LIST_DONE_RE = re.compile(rb"^\+OK LIST done (\d+)$")


def _dev_path_ok(rel: str) -> bool:
    if not rel or rel.startswith("/") or "\\" in rel:
        return False
    if ".." in rel.split("/"):
        return False
    if len(rel) >= 384:
        return False
    try:
        rel.encode("ascii")
    except UnicodeEncodeError:
        return False
    for part in rel.split("/"):
        if not part or part.startswith("."):
            return False
        if any(ord(c) < 0x20 for c in part):
            return False
    return True
_STATUS_INFO_RE = re.compile(
    rb"^\+OK STATUS sdcard=(yes|no) internal=(yes|no)$"
)
_LAST_PORT_LOGGED: str | None = None

def _strip_ansi(line: bytes) -> bytes:
    return _ANSI_RE.sub(b"", line)


def classify_line(line: bytes) -> str:
    line = _strip_ansi(line)
    if line.startswith(b"-ERR"):
        return "dev-err"
    if line.startswith(b"+"):
        return "dev-ok"
    if line.startswith(b"RELOAD done:") or line.startswith(b"RELOAD failed:"):
        return "espd"
    if _ESP_LOG_RE.match(line):
        return "esp-" + line[:1].decode().lower()
    return "pd"


def _styled(stream, text: str, kind: str) -> None:
    use_color = _out.color and stream.isatty()
    if use_color and kind in _STYLES and _STYLES[kind]:
        stream.write(f"{_STYLES[kind]}{text}{_RESET}\n")
    else:
        stream.write(text + "\n")
    stream.flush()


def log_script(text: str) -> None:
    _styled(sys.stderr, text, "script")


def log_dev_tx(cmd: str) -> None:
    _styled(sys.stderr, f"→ {cmd}", "dev-tx")


def log_dev_rx(line: bytes) -> None:
    text = line.decode(errors="replace")
    kind = classify_line(line)
    _styled(sys.stderr, f"← {text}", kind)


def log_device_line(line: bytes) -> None:
    kind = classify_line(line)
    text = line.decode(errors="replace")
    if kind.startswith("esp-"):
        if not _out.show_esp:
            return
        _styled(sys.stderr, text, kind)
    elif kind == "espd":
        _styled(sys.stderr, text, kind)
    else:
        _styled(sys.stderr, text, "pd")


def _need_serial():
    try:
        import serial  # noqa: F401
        import serial.serialutil
    except ImportError:
        sys.exit("pip install pyserial")


class EspdDisconnected(Exception):
    """CDC port went away (reset, unplug, or sleep)."""


def resolve_port(pattern: str) -> str:
    if any(ch in pattern for ch in "*?[]"):
        matches = sorted(glob.glob(pattern))
        if not matches:
            raise EspdDisconnected(f"no serial port matches {pattern!r}")
        if len(matches) > 1:
            sys.exit(
                f"ambiguous port {pattern!r} ({len(matches)} devices):\n"
                + "\n".join(f"  {m}" for m in matches)
                + "\nUse the OTG CDC port path explicitly (see docs/DEV_SYNC.md)."
            )
        return matches[0]
    if not os.path.exists(pattern):
        raise EspdDisconnected(f"serial port not found: {pattern!r}")
    return pattern


def autodetect_port() -> str:
    """Best-effort cross-platform CDC port detection."""
    from serial.tools import list_ports

    ports = list(list_ports.comports())
    if not ports:
        raise EspdDisconnected("no serial ports found")

    def _score(p) -> tuple[int, int]:
        dev = (p.device or "").lower()
        desc = (p.description or "").lower()
        hwid = (p.hwid or "").lower()
        s = 0
        if "esp" in desc or "esp" in hwid:
            s += 100
        if "cdc" in desc or "usb" in desc:
            s += 40
        if "usbmodem" in dev or "ttyacm" in dev or "ttyusb" in dev:
            s += 30
        if dev.startswith("com"):
            s += 20
        if p.vid is not None and p.pid is not None:
            s += 10
        # Prefer stable ordering if scores tie.
        return (s, -len(dev))

    ranked = sorted(ports, key=_score, reverse=True)
    best = ranked[0]
    best_score = _score(best)[0]
    if best_score <= 0:
        names = ", ".join(p.device for p in ports)
        raise EspdDisconnected(f"could not infer CDC port from: {names}")

    # If multiple candidates are similarly good, require explicit port.
    top = [p for p in ranked if _score(p)[0] == best_score]
    if len(top) > 1:
        choices = "\n".join(
            f"  {p.device} ({p.description or 'unknown'})" for p in top
        )
        raise EspdDisconnected(
            "multiple likely CDC ports found; pass --port explicitly:\n" + choices
        )
    return best.device


def confirm_watch_dir_has_main(watch_dir: str, *, yes: bool) -> None:
    main_pd = os.path.join(watch_dir, "main.pd")
    if os.path.isfile(main_pd):
        return
    log_script(f"warning: no main.pd in {watch_dir}")
    if yes:
        log_script("continuing without main.pd (--yes)")
        return
    if not (sys.stdin.isatty() and sys.stderr.isatty()):
        sys.exit("aborting: no main.pd (use --yes to continue non-interactively)")
    try:
        answer = input("Continue sync anyway? [y/N]: ").strip().lower()
    except EOFError:
        answer = ""
    if answer not in ("y", "yes"):
        sys.exit("aborted")


def _serial_open_retry(port: str, timeout: float):
    import serial
    import serial.serialutil

    deadline = time.monotonic() + timeout
    last_err: Exception | None = None
    while time.monotonic() < deadline:
        try:
            return serial.Serial(port, _SERIAL_BAUD, timeout=0.05)
        except (serial.serialutil.SerialException, OSError) as e:
            last_err = e
            msg = str(e).lower()
            errno = getattr(e, "errno", None)
            if (
                errno in (2, 6, 16)
                or "not configured" in msg
                or "resource busy" in msg
            ):
                time.sleep(0.4)
                continue
            raise
    raise EspdDisconnected(
        f"serial port not ready after {timeout:.0f}s: {port} ({last_err})"
    )


def wait_port_gone(pattern: str, timeout: float = 20.0) -> None:
    """After reboot, wait until the old CDC device node disappears (macOS re-enumerate)."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            port = resolve_port(pattern)
        except EspdDisconnected:
            return
        if not os.path.exists(port):
            return
        time.sleep(0.2)
    log_script("note: CDC port still present; continuing anyway")


def wait_serial_ready(pattern: str, timeout: float = 45.0) -> str:
    """Wait until the CDC port exists and accepts an open (post-reboot re-enumeration)."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        try:
            port = resolve_port(pattern)
        except EspdDisconnected:
            time.sleep(0.3)
            continue
        try:
            ser = _serial_open_retry(port, min(8.0, remaining))
            ser.close()
            return port
        except EspdDisconnected:
            time.sleep(0.4)
    raise TimeoutError(f"CDC port not ready within {timeout:.0f}s ({pattern!r})")


def wifi_exchange(
    host: str, text: str, port: int = 4499, timeout: float = 10.0
) -> bytes:
    """One-shot command (used internally; prefer EspdWifi)."""
    import socket

    payload = (text if text.endswith("\n") else text + "\n").encode()
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(payload)
        buf = b""
        while b"\n" not in buf:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
        line, _, _ = buf.partition(b"\n")
        return line.strip(b"\r")


@dataclass
class SyncTarget:
    wifi_host: str | None = None
    port_pattern: str | None = None
    wifi_port: int = 4499

    @classmethod
    def serial(cls, pattern: str) -> SyncTarget:
        return cls(port_pattern=pattern)

    @classmethod
    def wifi(cls, host: str, port: int = 4499) -> SyncTarget:
        return cls(wifi_host=host, wifi_port=port)

    def reconnect_prepared(self, *, exit_on_fail: bool = True) -> EspdCdc | EspdWifi:
        if self.wifi_host:
            return connect_wifi_and_prepare(
                self.wifi_host, self.wifi_port, exit_on_fail=exit_on_fail
            )
        return connect_and_prepare(self.port_pattern, exit_on_fail=exit_on_fail)


class _EspdSyncBase:
    """Shared espd_dev line protocol over a byte stream (serial or WiFi TCP)."""

    last_status: bytes | None

    def __init__(self) -> None:
        self.last_status = None
        self._lock = threading.Lock()
        self._cmd_lock = threading.Lock()
        self._reply: bytes | None = None
        self._reply_event = threading.Event()
        self._stop = threading.Event()
        self._disconnected = threading.Event()
        self._put_active = False
        self._put_total_bytes = 0
        self._put_rel_path = ""
        self._put_last_pct = -1
        self._list_active = False
        self._list_paths: list[str] = []
        self._awaiting_reply = False
        self._thread = threading.Thread(target=self._reader, daemon=True)

    def _start_reader(self) -> None:
        self._thread.start()

    @property
    def alive(self) -> bool:
        return not self._disconnected.is_set() and not self._stop.is_set()

    def _mark_disconnected(self, reason: str) -> None:
        if self._disconnected.is_set():
            return
        self._disconnected.set()
        self._reply_event.set()
        log_script(f"disconnected: {reason}")

    def close(self) -> None:
        self._stop.set()
        self._reply_event.set()
        if self._thread.is_alive():
            self._thread.join(timeout=1.0)
        self._close_io()

    def _close_io(self) -> None:
        raise NotImplementedError

    def _read_chunk(self) -> bytes:
        raise NotImplementedError

    def _write_bytes(self, data: bytes) -> None:
        raise NotImplementedError

    def _handle_line(self, line: bytes) -> None:
        if self._put_active:
            if (
                line.startswith(b"+OK PUT ack")
                or line.startswith(b"+OK PUT done")
                or line.startswith(b"-ERR")
            ):
                with self._lock:
                    self._reply = line
                self._reply_event.set()
                if line.startswith(b"+OK PUT ack"):
                    m = _PUT_ACK_RE.match(line.strip())
                    if m and self._put_total_bytes > 0:
                        acked = int(m.group(1), 10)
                        pct = round((acked / self._put_total_bytes) * 100)
                        if pct != self._put_last_pct:
                            self._put_last_pct = pct
                            sys.stderr.write(f"\r{self._put_rel_path}{pct}%")
                            sys.stderr.flush()
                elif line.startswith(b"+OK PUT done"):
                    if self._put_last_pct >= 0:
                        sys.stderr.write("\n")
                        sys.stderr.flush()
                    log_dev_rx(line)
                else:
                    log_dev_rx(line)
            else:
                log_device_line(line)
            return
        if self._list_active:
            if line.startswith(b"+FILE "):
                raw = line[6:]
                try:
                    rel = raw.decode("ascii")
                except UnicodeDecodeError:
                    log_script(f"warning: ignore device path {raw!r}")
                else:
                    if _dev_path_ok(rel):
                        with self._lock:
                            self._list_paths.append(rel)
                    else:
                        log_script(f"warning: ignore device path {raw!r}")
                log_dev_rx(line)
                return
            proto = _extract_protocol_line(line)
            if proto is not None:
                if proto != line:
                    idx = line.find(proto)
                    if idx > 0:
                        log_device_line(line[:idx].rstrip(b"\r"))
                if proto.startswith(b"+OK LIST done") or proto.startswith(b"-ERR"):
                    with self._lock:
                        self._reply = proto
                    self._reply_event.set()
                    log_dev_rx(proto)
                elif proto.startswith(b"+OK LIST"):
                    log_dev_rx(proto)
            else:
                log_device_line(line)
            return
        proto = _extract_protocol_line(line)
        if proto is not None:
            if proto != line:
                idx = line.find(proto)
                if idx > 0:
                    log_device_line(line[:idx].rstrip(b"\r"))
            log_dev_rx(proto)
            if self._awaiting_reply:
                with self._lock:
                    self._reply = proto
                self._reply_event.set()
        else:
            log_device_line(line)

    def _reader(self) -> None:
        buf = b""
        while not self._stop.is_set():
            try:
                chunk = self._read_chunk()
            except EspdDisconnected as e:
                if self._stop.is_set():
                    break
                self._mark_disconnected(str(e))
                break
            if not chunk:
                time.sleep(0.01)
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.strip(b"\r")
                if line:
                    self._handle_line(line)

    def _check_alive(self) -> None:
        if not self.alive:
            raise EspdDisconnected("device link closed")

    def _wait_reply(self, timeout: float) -> bytes:
        if not self._reply_event.wait(timeout):
            self._check_alive()
            raise TimeoutError("device reply timeout")
        self._check_alive()
        with self._lock:
            line = self._reply or b""
            self._reply = None
        self._reply_event.clear()
        return line

    def command(self, text: str, timeout: float = 10.0) -> bytes:
        self._check_alive()
        with self._cmd_lock:
            self._reply_event.clear()
            with self._lock:
                self._reply = None
            payload = text if text.endswith("\n") else text + "\n"
            log_dev_tx(text.strip())
            self._awaiting_reply = True
            try:
                try:
                    self._write_bytes(payload.encode())
                except EspdDisconnected:
                    raise
                except OSError as e:
                    self._mark_disconnected(str(e))
                    raise EspdDisconnected(str(e)) from e
                return self._wait_reply(timeout)
            finally:
                self._awaiting_reply = False

    def put_file(self, local_path: str, rel_path: str, crc: int) -> bool:
        data = open(local_path, "rb").read()
        rel_path = rel_path.replace(os.sep, "/")
        nbytes = len(data)
        probe_timeout = max(20.0, nbytes / 50000.0)
        done_timeout = max(60.0, nbytes / 8000.0)
        line = b""
        for attempt in range(2):
            try:
                line = self.command(
                    f"PUT {rel_path} {nbytes} {crc:08x}", timeout=probe_timeout
                )
                break
            except TimeoutError:
                if attempt == 0:
                    log_script(f"PUT {rel_path}: no reply, retrying…")
                    self._reply_event.clear()
                    continue
                raise
        if line.startswith(b"+OK PUT skip"):
            return False
        try:
            put_window = _put_window_from_ready(line)
        except RuntimeError:
            if line.startswith(b"-ERR"):
                raise RuntimeError(line.decode(errors="replace"))
            if line.strip().startswith(b"+OK PUT ready"):
                raise RuntimeError("PUT ready missing window= (update firmware)")
            raise
        ack_timeout = max(30.0, nbytes / 40000.0)
        log_script(f"sending {nbytes} bytes for {rel_path} (window {put_window})")
        try:
            with self._cmd_lock:
                self._put_active = True
                self._put_total_bytes = nbytes
                self._put_rel_path = f"{rel_path}: "
                self._put_last_pct = -1
                for off in range(0, len(data), put_window):
                    part = data[off : off + put_window]
                    self._reply_event.clear()
                    with self._lock:
                        self._reply = None
                    self._write_bytes(part)
                    line = self._wait_reply(ack_timeout)
                    m = _PUT_ACK_RE.match(line.strip())
                    if not m:
                        if line.startswith(b"-ERR"):
                            raise RuntimeError(line.decode(errors="replace"))
                        raise RuntimeError(
                            f"unexpected PUT reply: {line.decode(errors='replace')}"
                        )
                    acked = int(m.group(1), 10)
                    if acked != off + len(part):
                        raise RuntimeError(
                            f"PUT ack mismatch: expected {off + len(part)}, got {acked}"
                        )
        except EspdDisconnected:
            self._put_active = False
            raise
        except OSError as e:
            self._put_active = False
            self._mark_disconnected(str(e))
            raise EspdDisconnected(str(e)) from e
        try:
            line = self._wait_reply(done_timeout)
            m = _PUT_DONE_RE.match(line.strip())
            if m and int(m.group(1), 16) == crc:
                return True
            if line.startswith(b"-ERR"):
                raise RuntimeError(line.decode(errors="replace"))
            raise RuntimeError(f"unexpected PUT reply: {line.decode(errors='replace')}")
        finally:
            self._put_active = False

    def reload(self) -> bytes:
        return self.command("RELOAD", timeout=30.0)

    def send_pd(self, message: str) -> bytes:
        msg = message.strip()
        if not msg:
            raise ValueError("empty Pd message")
        return self.command(f"MSG {msg}", timeout=5.0)

    def reset_device(self) -> None:
        try:
            self.command("RESET", timeout=2.0)
        except (EspdDisconnected, TimeoutError):
            pass

    def status(self, timeout: float = 10.0) -> bytes:
        return self.command("STATUS", timeout=timeout)

    def list_files(self, timeout: float = 120.0) -> list[str]:
        self._check_alive()
        with self._cmd_lock:
            self._list_paths = []
            self._list_active = True
            self._reply_event.clear()
            with self._lock:
                self._reply = None
            log_dev_tx("LIST")
            try:
                self._write_bytes(b"LIST\n")
            except EspdDisconnected:
                raise
            except OSError as e:
                self._list_active = False
                self._mark_disconnected(str(e))
                raise EspdDisconnected(str(e)) from e
            try:
                line = self._wait_reply(timeout)
            finally:
                self._list_active = False
        if line.startswith(b"-ERR"):
            raise RuntimeError(line.decode(errors="replace"))
        m = _LIST_DONE_RE.match(line.strip())
        if not m:
            raise RuntimeError(f"unexpected LIST reply: {line.decode(errors='replace')}")
        return list(self._list_paths)

    def rm_file(self, rel_path: str, timeout: float = 10.0) -> None:
        rel_path = rel_path.replace(os.sep, "/")
        line = self.command(f"RM {rel_path}", timeout=timeout)
        if line.startswith(b"-ERR"):
            raise RuntimeError(line.decode(errors="replace"))


class EspdCdc(_EspdSyncBase):
    def __init__(self, port: str, open_timeout: float = 8.0):
        super().__init__()
        self.port = port
        self._ser = _serial_open_retry(port, open_timeout)
        self._start_reader()

    def _close_io(self) -> None:
        try:
            if self._ser.is_open:
                self._ser.close()
        except Exception:
            pass

    def _read_chunk(self) -> bytes:
        import serial.serialutil

        try:
            return self._ser.read(4096)
        except (serial.serialutil.SerialException, OSError) as e:
            raise EspdDisconnected(str(e)) from e

    def _write_bytes(self, data: bytes) -> None:
        self._ser.write(data)
        self._ser.flush()


class EspdWifi(_EspdSyncBase):
    def __init__(self, host: str, port: int = 4499, connect_timeout: float = 8.0):
        import socket

        super().__init__()
        self.host = host
        self.port = port
        self._sock = socket.create_connection((host, port), timeout=connect_timeout)
        self._sock.settimeout(0.05)
        self._start_reader()

    def _close_io(self) -> None:
        try:
            self._sock.close()
        except Exception:
            pass

    def _read_chunk(self) -> bytes:
        import socket

        try:
            return self._sock.recv(4096)
        except socket.timeout:
            return b""
        except OSError as e:
            raise EspdDisconnected(str(e)) from e

    def _write_bytes(self, data: bytes) -> None:
        self._sock.sendall(data)


EspdClient = EspdCdc | EspdWifi


def _extract_protocol_line(line: bytes) -> bytes | None:
    """Host line may be glued to a log prefix (e.g. b'I+OK STATUS ...')."""
    if line.startswith(b"+") or line.startswith(b"-ERR"):
        return line
    for prefix in (b"+OK ", b"+OK", b"-ERR"):
        idx = line.find(prefix)
        if idx >= 0:
            return line[idx:]
    return None


def parse_status_info(line: bytes) -> dict[str, str]:
    proto = _extract_protocol_line(line) or line
    m = _STATUS_INFO_RE.match(proto.strip()) or _STATUS_INFO_RE.search(proto)
    if not m:
        raise RuntimeError(f"unexpected STATUS reply: {line.decode(errors='replace')}")
    return {
        "sdcard": m.group(1).decode(),
        "internal": m.group(2).decode(),
    }


def explain_status(info: dict[str, str]) -> str | None:
    """Human note when internal=no."""
    if info["internal"] == "yes":
        return None
    return "note: /storage not available on device (drive mode or boot in progress)"


def resolve_port_pattern(port_arg: str | None) -> str:
    """Return port path or glob; log once when auto-detected."""
    global _LAST_PORT_LOGGED
    if port_arg:
        return port_arg
    port = autodetect_port()
    if port != _LAST_PORT_LOGGED:
        log_script(f"port: {port}")
        _LAST_PORT_LOGGED = port
    return port


def device_status(client: EspdClient) -> dict[str, str]:
    if client.last_status and client.last_status.startswith(b"+OK STATUS"):
        return parse_status_info(client.last_status)
    return parse_status_info(client.status())


def sync_store_path(info: dict[str, str]) -> str:
    return "/sdcard" if info["sdcard"] == "yes" else "/storage"


def prepare_for_sync(client: EspdClient, target: SyncTarget) -> EspdClient:
    """SD when a card is mounted; otherwise sync to internal flash (/storage)."""
    info = device_status(client)
    if info["sdcard"] == "yes":
        log_script("SD card available -- using /sdcard")
        return client
    if info["internal"] == "yes":
        return client
    if target.wifi_host:
        return ensure_storage_for_write_wifi(client, target)
    return ensure_storage_for_write(client, target.port_pattern)


def wait_for_storage_ready(client: EspdClient, timeout_s: float = 45.0) -> dict[str, str]:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        info = device_status(client)
        if info["internal"] == "yes":
            return info
        log_script("waiting for /storage on device…")
        time.sleep(0.5)
    raise RuntimeError("/storage not ready on device (boot still in progress?)")


def ensure_storage_for_write_wifi(client: EspdWifi, target: SyncTarget) -> EspdWifi:
    """Reset the device so /storage is APP-mounted (exits drive mode)."""
    log_script("internal storage not available -- resetting device")
    client.reset_device()
    client.close()
    log_script("waiting for WiFi sync after reboot (up to 60s)…")
    time.sleep(2.0)
    client = connect_wifi(target.wifi_host, target.wifi_port, ready_timeout=60.0)
    info = wait_for_storage_ready(client)
    if info["internal"] != "yes":
        raise RuntimeError(
            f"/storage still not available after reset (internal={info['internal']})"
        )
    return client


def ensure_storage_for_write(client: EspdCdc, port: str) -> EspdCdc:
    """Reset the device so /storage is APP-mounted (exits drive mode)."""
    log_script("internal storage in drive mode -- resetting device")
    cdc.reset_device()
    cdc.close()
    wait_port_gone(port, timeout=25.0)
    log_script("waiting for CDC after reboot (up to 60s)…")
    cdc = connect_cdc(port, ready_timeout=60.0)
    info = wait_for_storage_ready(cdc)
    if info["internal"] != "yes":
        raise RuntimeError(
            f"/storage still not available after reset (internal={info['internal']})"
        )
    return cdc


def connect_wifi(host: str, port: int = 4499, ready_timeout: float = 8.0) -> EspdWifi:
    deadline = time.monotonic() + ready_timeout
    last_err = ""
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        client: EspdWifi | None = None
        try:
            client = EspdWifi(host, port, connect_timeout=min(remaining, 8.0))
            for _ in range(5):
                try:
                    last = client.status(timeout=3.0)
                    if b"+OK STATUS" in last:
                        log_script(
                            f"connected ({host}:{port}): "
                            f"{last.decode(errors='replace')}"
                        )
                        connected = client
                        connected.last_status = last
                        client = None
                        return connected
                except EspdDisconnected as e:
                    last_err = str(e)
                    break
                except TimeoutError:
                    pass
        except (TimeoutError, EspdDisconnected, OSError) as e:
            last_err = str(e)
        finally:
            if client is not None:
                client.close()
        time.sleep(0.5)
    raise TimeoutError(
        f"WiFi connect failed within {ready_timeout:.0f}s"
        + (f" ({last_err})" if last_err else "")
    )


def connect_wifi_and_prepare(
    host: str, port: int = 4499, *, exit_on_fail: bool = False
) -> EspdWifi:
    """Connect over WiFi TCP and prepare SD or internal-flash sync path."""
    target = SyncTarget.wifi(host, port)
    while True:
        try:
            client = connect_wifi(host, port, ready_timeout=60.0)
            try:
                return prepare_for_sync(client, target)
            except Exception:
                client.close()
                raise
        except (TimeoutError, EspdDisconnected, RuntimeError, OSError) as e:
            log_script(f"waiting for device ({e})…")
            if exit_on_fail:
                raise
            time.sleep(1.5)


def connect_cdc(port_pattern: str, ready_timeout: float = 8.0) -> EspdCdc:
    import serial.serialutil
    global _LAST_PORT_LOGGED

    deadline = time.monotonic() + ready_timeout
    last_err = ""
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        cdc: EspdCdc | None = None
        try:
            port = wait_serial_ready(port_pattern, timeout=remaining)
            if port != _LAST_PORT_LOGGED:
                log_script(f"port: {port}")
                _LAST_PORT_LOGGED = port
            cdc = EspdCdc(port)
            for _ in range(5):
                try:
                    last = cdc.status(timeout=3.0)
                    if b"+OK STATUS" in last:
                        log_script(f"connected ({port}): {last.decode(errors='replace')}")
                        connected = cdc
                        connected.last_status = last
                        cdc = None
                        return connected
                except EspdDisconnected as e:
                    last_err = str(e)
                    break
                except TimeoutError:
                    pass
        except (TimeoutError, EspdDisconnected, serial.serialutil.SerialException, OSError) as e:
            last_err = str(e)
        finally:
            if cdc is not None:
                cdc.close()
        time.sleep(0.5)
    raise TimeoutError(
        f"CDC connect failed within {ready_timeout:.0f}s"
        + (f" ({last_err})" if last_err else "")
    )


def connect_and_prepare(
    port_pattern: str, *, exit_on_fail: bool = False
) -> EspdCdc:
    """Connect over CDC and prepare SD or internal-flash sync path."""
    target = SyncTarget.serial(port_pattern)
    while True:
        try:
            cdc = connect_cdc(port_pattern, ready_timeout=60.0)
            try:
                return prepare_for_sync(cdc, target)
            except Exception:
                cdc.close()
                raise
        except (TimeoutError, EspdDisconnected, RuntimeError, OSError) as e:
            log_script(f"waiting for device ({e})…")
            if exit_on_fail:
                raise
            time.sleep(1.5)


def local_file_hash(path: str) -> tuple[int, int]:
    crc = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            crc = zlib.crc32(chunk, crc)
    return os.path.getsize(path), crc & 0xFFFFFFFF


def mirror_prune(client: EspdClient, keep: set[str], target: SyncTarget) -> EspdClient:
    keep_norm = {r.replace(os.sep, "/") for r in keep}
    while True:
        try:
            on_device = set(client.list_files())
            break
        except (EspdDisconnected, TimeoutError) as e:
            kind = "timeout" if isinstance(e, TimeoutError) else "disconnect"
            log_script(f"{kind} during LIST; reconnecting…")
            client.close()
            client = target.reconnect_prepared()
    orphans = sorted(p for p in (on_device - keep_norm) if _dev_path_ok(p))
    skipped = sorted(p for p in (on_device - keep_norm) if not _dev_path_ok(p))
    for rel in skipped:
        log_script(f"warning: cannot mirror-remove invalid device path {rel!r}")
    if not orphans:
        return client
    log_script(f"mirror: removing {len(orphans)} file(s) not in project")
    for rel in orphans:
        while True:
            try:
                log_script(f"remove {rel}")
                client.rm_file(rel)
                break
            except (EspdDisconnected, TimeoutError) as e:
                kind = "timeout" if isinstance(e, TimeoutError) else "disconnect"
                log_script(f"{kind} during RM {rel}; reconnecting…")
                client.close()
                client = target.reconnect_prepared()
            except RuntimeError as e:
                msg = str(e)
                if "not mounted" in msg:
                    log_script("storage not ready during RM; reconnecting…")
                    client.close()
                    client = target.reconnect_prepared()
                    continue
                if "not found" in msg or "bad path" in msg:
                    log_script(f"skip {rel} ({msg.strip()})")
                    break
                raise
    return client


def sync_files(
    client: EspdClient,
    watch_dir: str,
    rels: list[str],
    target: SyncTarget,
    *,
    mirror: bool = True,
) -> EspdClient:
    if mirror:
        client = mirror_prune(client, set(rels), target)
    reload_needed = False
    reset_needed = False
    uploaded = 0
    skipped = 0
    t0 = time.time()

    for rel in sorted(rels):
        local = os.path.join(watch_dir, rel.replace("/", os.sep))
        _size, crc = local_file_hash(local)
        while True:
            try:
                sent = client.put_file(local, rel, crc)
            except (EspdDisconnected, TimeoutError) as e:
                kind = "timeout" if isinstance(e, TimeoutError) else "disconnect"
                log_script(f"{kind} during PUT {rel}; reconnecting…")
                client.close()
                try:
                    client = target.reconnect_prepared()
                except (TimeoutError, EspdDisconnected, RuntimeError) as re_err:
                    log_script(f"reconnect failed during PUT {rel}: {re_err}")
                    time.sleep(1.0)
                    continue
                continue
            except RuntimeError as e:
                if "crc mismatch" in str(e) or "crc exp" in str(e):
                    log_script(f"PUT verify failed for {rel}, retrying…")
                    continue
                if "not mounted" in str(e):
                    log_script("storage not ready during PUT; reconnecting…")
                    client.close()
                    client = target.reconnect_prepared()
                    continue
                if "no space" in str(e):
                    log_script(f"skip {rel} (device full)")
                    break
                raise
            if sent:
                uploaded += 1
                if rel == "config.txt":
                    reset_needed = True
                else:
                    reload_needed = True
            else:
                skipped += 1
            break
    if reset_needed:
        log_script("RESET (config.txt applies on boot)")
        try:
            client.reset_device()
        except (EspdDisconnected, TimeoutError):
            pass
        try:
            client.close()
        except Exception:
            pass
        try:
            client = target.reconnect_prepared(exit_on_fail=False)
        except (TimeoutError, EspdDisconnected, RuntimeError) as e:
            log_script(f"reconnect after RESET: {e}")
            client = None
    elif reload_needed:
        log_script("RELOAD")
        try:
            client.reload()
        except (EspdDisconnected, TimeoutError):
            log_script("timeout/disconnect during RELOAD (patch may still reload on device)")
    log_script(
        f"sync done in {time.time() - t0:.2f}s "
        f"({uploaded} uploaded, {skipped} unchanged)"
    )
    try:
        client.status()
    except (EspdDisconnected, TimeoutError):
        pass
    return client


def collect_files(root: str) -> dict[str, float]:
    out: dict[str, float] = {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if not d.startswith(".")]
        for name in filenames:
            if not name or name.startswith("."):
                continue
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            out[rel] = os.path.getmtime(full)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="ESPD rapid sync over CDC or WiFi SoftAP")
    ap.add_argument(
        "--host",
        metavar="IP",
        help="device SoftAP address for WiFi sync (default serial); e.g. 192.168.4.1",
    )
    ap.add_argument(
        "--wifi-port",
        type=int,
        default=4499,
        help="TCP sync port when using --host (default: 4499)",
    )
    ap.add_argument("-p", "--port", help="CDC serial port path (auto-detect if omitted)")
    ap.add_argument(
        "watch_dir",
        nargs="?",
        default=".",
        help="Local folder to watch (default: current directory)",
    )
    ap.add_argument(
        "-y",
        "--yes",
        action="store_true",
        help="continue even when watch folder has no main.pd",
    )
    ap.add_argument("--debounce", type=float, default=0.35, help="seconds after save")
    ap.add_argument("--status", action="store_true", help="STATUS and exit")
    ap.add_argument(
        "--pd-msg",
        metavar="TEXT",
        help="send one MSG to Pd and exit (e.g. '; pd dsp 1' or 'print hello')",
    )
    ap.add_argument("--reset", action="store_true", help="RESET device and exit")
    ap.add_argument(
        "--reload",
        action="store_true",
        help="RELOAD main.pd from active store and exit",
    )
    ap.add_argument("--no-color", action="store_true", help="disable ANSI colors")
    ap.add_argument(
        "--no-esp-log",
        action="store_true",
        help="hide ESP-IDF log lines (I/W/E tag: messages)",
    )
    ap.add_argument(
        "--no-reconnect",
        action="store_true",
        help="exit when the device disconnects instead of waiting to reconnect",
    )
    ap.add_argument(
        "--no-initial-sync",
        action="store_true",
        help="skip the sync pass on connect (watch for saves only)",
    )
    ap.add_argument(
        "--no-mirror",
        action="store_true",
        help="do not remove device files missing from the local project (default: mirror)",
    )
    ap.add_argument(
        "--resync-on-reconnect",
        action="store_true",
        help="after reconnect/reset, run the same PUT/sync pass again",
    )
    args = ap.parse_args()

    _out.color = not args.no_color
    _out.show_esp = not args.no_esp_log

    if args.host:
        target = SyncTarget.wifi(args.host, args.wifi_port)
    else:
        _need_serial()
        try:
            port_pattern = resolve_port_pattern(args.port)
        except EspdDisconnected as e:
            sys.exit(f"port detect failed: {e}")
        target = SyncTarget.serial(port_pattern)

    watch_dir = os.path.abspath(args.watch_dir)
    if not os.path.isdir(watch_dir):
        sys.exit(f"not a directory: {watch_dir}")
    confirm_watch_dir_has_main(watch_dir, yes=args.yes)

    client: EspdClient | None = None
    try:
        def run_sync(label: str) -> None:
            nonlocal client
            rels = list(collect_files(watch_dir))
            if not rels:
                log_script(f"{label}: nothing to send")
                return
            log_script(f"{label} ({len(rels)} files)")
            client = sync_files(
                client, watch_dir, rels, target, mirror=not args.no_mirror
            )

        connect_timeout = 30.0

        if args.reset:
            try:
                if target.wifi_host:
                    client = connect_wifi(
                        target.wifi_host, target.wifi_port, ready_timeout=connect_timeout
                    )
                else:
                    client = connect_cdc(target.port_pattern, ready_timeout=connect_timeout)
                client.reset_device()
            except (TimeoutError, EspdDisconnected):
                pass
            client = None
            if args.no_reconnect:
                return 0
            client = target.reconnect_prepared(exit_on_fail=False)
            if not args.no_initial_sync:
                run_sync("sync after reset")
            return 0

        if args.reload:
            if target.wifi_host:
                client = connect_wifi(
                    target.wifi_host, target.wifi_port, ready_timeout=connect_timeout
                )
            else:
                client = connect_cdc(target.port_pattern, ready_timeout=connect_timeout)
            line = client.reload()
            log_script(line.decode(errors="replace").strip())
            return 0

        if args.status:
            if target.wifi_host:
                client = connect_wifi(
                    target.wifi_host, target.wifi_port, ready_timeout=connect_timeout
                )
            else:
                client = connect_cdc(target.port_pattern, ready_timeout=connect_timeout)
            line = client.last_status or client.status()
            log_script(line.decode(errors="replace"))
            info = device_status(client)
            note = explain_status(info)
            if note:
                log_script(note)
            store = sync_store_path(info)
            log_script(f"sync target would be {store}")
            return 0

        if args.pd_msg:
            if target.wifi_host:
                client = connect_wifi(
                    target.wifi_host, target.wifi_port, ready_timeout=connect_timeout
                )
            else:
                client = connect_cdc(target.port_pattern, ready_timeout=connect_timeout)
            line = client.send_pd(args.pd_msg)
            log_script(line.decode(errors="replace").strip())
            return 0

        client = target.reconnect_prepared(exit_on_fail=False)
        store = sync_store_path(device_status(client))

        if not args.no_initial_sync:
            run_sync("sync")

        mtimes = collect_files(watch_dir)
        log_script(
            f"watching {watch_dir} ({len(mtimes)} files) — sync to {store}"
        )

        while True:
            if client is None or not client.alive:
                if args.no_reconnect:
                    log_script("device disconnected — exiting")
                    return 1
                if client is not None:
                    client.close()
                client = None
                prev_store = store
                try:
                    client = target.reconnect_prepared(exit_on_fail=False)
                except (TimeoutError, EspdDisconnected, RuntimeError) as e:
                    log_script(str(e))
                    time.sleep(1.0)
                    continue
                store = sync_store_path(device_status(client))
                if store != prev_store:
                    log_script(f"sync target: {store}")
                    if not args.no_initial_sync:
                        run_sync("sync after reconnect")
                    mtimes = collect_files(watch_dir)
                elif args.resync_on_reconnect and not args.no_initial_sync:
                    run_sync("resync after reconnect")
                    mtimes = collect_files(watch_dir)

            time.sleep(0.2)
            try:
                now = collect_files(watch_dir)
                changed = [rel for rel in now if rel not in mtimes or now[rel] != mtimes[rel]]
                if not changed:
                    continue
                time.sleep(args.debounce)
                now2 = collect_files(watch_dir)
                changed = [
                    rel for rel in now2 if rel not in mtimes or now2[rel] != mtimes[rel]
                ]
                if not changed:
                    continue
                client = sync_files(
                    client, watch_dir, changed, target, mirror=False
                )
                mtimes = now2
            except (EspdDisconnected, TimeoutError):
                log_script("timeout/disconnect during watch sync; staying in watch mode")
                try:
                    client.close()
                except Exception:
                    pass
                client = None
                continue
            except RuntimeError as e:
                log_script(f"sync error: {e}; staying in watch mode")
                try:
                    client.close()
                except Exception:
                    pass
                client = None
                continue

    except KeyboardInterrupt:
        log_script("stopped")
        return 0
    finally:
        if client:
            client.close()


if __name__ == "__main__":
    sys.exit(main())
