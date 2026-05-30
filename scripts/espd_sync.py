#!/usr/bin/env python3
"""
Watch a local Pure Data project folder and sync to ESP local storage over CDC.

Requires: pip install pyserial

Usage:
  python3 scripts/espd_sync.py [-p PORT] [./my_patch]

PORT is the OTG CDC serial device (pyserial): e.g. /dev/cu.usbmodem* (macOS),
/dev/ttyACM0 (Linux), COM3 (Windows). See docs/DEV_SYNC.md.

The ESP must have CONFIG_ESPD_DEV_CDC_SYNC for dev sync.
Do not run idf.py monitor on the same port at the same time.
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
_STATUS_INFO_RE = re.compile(
    rb"^\+OK STATUS sdcard=(yes|no) internal=(yes|no) mode=(normal|msc_sync)$"
)
_LAST_PORT_LOGGED: str | None = None

# USB CDC ignores baud. Pace payload so TinyUSB can drain (raise if stable on your port).
# ~800k BPS helps SD a lot; MSC bulk is usually limited by flash write/fsync on device.
_PUT_STREAM_BPS = 800000.0
_PUT_STREAM_CHUNK = 4096
_SERIAL_BAUD = 921600


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
        _styled(sys.stdout, text, kind)
    elif kind == "espd":
        _styled(sys.stderr, text, kind)
    else:
        _styled(sys.stdout, text, "pd")


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
            ser.dtr = True
            ser.rts = True
            ser.close()
            return port
        except EspdDisconnected:
            time.sleep(0.4)
    raise TimeoutError(f"CDC port not ready within {timeout:.0f}s ({pattern!r})")


class EspdCdc:
    def __init__(self, port: str, open_timeout: float = 8.0):
        self.port = port
        self.last_status: bytes | None = None
        self._ser = _serial_open_retry(port, open_timeout)
        self._ser.dtr = True
        self._ser.rts = True

        self._lock = threading.Lock()
        self._cmd_lock = threading.Lock()
        self._reply: bytes | None = None
        self._reply_event = threading.Event()
        self._stop = threading.Event()
        self._disconnected = threading.Event()
        self._put_active = False
        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()

    @property
    def alive(self) -> bool:
        return not self._disconnected.is_set() and not self._stop.is_set()

    def _mark_disconnected(self, reason: str) -> None:
        if self._disconnected.is_set():
            return
        self._disconnected.set()
        self._reply_event.set()
        reason_l = reason.lower()
        if "not configured" in reason_l:
            log_script(f"disconnected (USB re-enumeration): {reason}")
        else:
            log_script(f"disconnected: {reason}")

    def close(self) -> None:
        self._stop.set()
        self._reply_event.set()
        if self._thread.is_alive():
            self._thread.join(timeout=1.0)
        try:
            if self._ser.is_open:
                self._ser.close()
        except Exception:
            pass

    def _reader(self) -> None:
        import serial.serialutil

        buf = b""
        while not self._stop.is_set():
            try:
                chunk = self._ser.read(4096)
            except (serial.serialutil.SerialException, OSError) as e:
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
                if not line:
                    continue
                if self._put_active:
                    if line.startswith(b"+OK PUT done") or line.startswith(b"-ERR"):
                        with self._lock:
                            self._reply = line
                        self._reply_event.set()
                        log_dev_rx(line)
                    continue
                if line.startswith(b"+") or line.startswith(b"-ERR"):
                    with self._lock:
                        self._reply = line
                    self._reply_event.set()
                    log_dev_rx(line)
                else:
                    log_device_line(line)

    def _check_alive(self) -> None:
        if not self.alive:
            raise EspdDisconnected("serial port closed")

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
            try:
                self._ser.write(payload.encode())
                self._ser.flush()
            except OSError as e:
                self._mark_disconnected(str(e))
                raise EspdDisconnected(str(e)) from e
            return self._wait_reply(timeout)

    def put_file(self, local_path: str, rel_path: str, crc: int) -> bool:
        """PUT with CRC. Returns True if bytes were sent, False if device skipped."""
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
        if not line.startswith(b"+OK PUT ready"):
            if line.startswith(b"-ERR"):
                raise RuntimeError(line.decode(errors="replace"))
            raise RuntimeError(f"unexpected PUT reply: {line.decode(errors='replace')}")
        log_script(f"sending {nbytes} bytes for {rel_path}")
        try:
            with self._cmd_lock:
                self._reply_event.clear()
                with self._lock:
                    self._reply = None
                self._put_active = True
                next_send = time.monotonic()
                for off in range(0, len(data), _PUT_STREAM_CHUNK):
                    part = data[off : off + _PUT_STREAM_CHUNK]
                    self._ser.write(part)
                    next_send += len(part) / _PUT_STREAM_BPS
                    sleep_for = next_send - time.monotonic()
                    if sleep_for > 0:
                        time.sleep(sleep_for)
                self._ser.flush()
        except OSError as e:
            self._put_active = False
            self._mark_disconnected(str(e))
            raise EspdDisconnected(str(e)) from e
        try:
            while True:
                line = self._wait_reply(done_timeout)
                m = _PUT_DONE_RE.match(line.strip())
                if m and int(m.group(1), 16) == crc:
                    return True
                if line.startswith(b"-ERR"):
                    raise RuntimeError(line.decode(errors="replace"))
                raise RuntimeError(f"unexpected PUT reply: {line.decode(errors='replace')}")
        finally:
            self._put_active = False

    def reload(self) -> None:
        self.command("RELOAD", timeout=30.0)

    def send_pd(self, message: str) -> bytes:
        """Send one Pd message (semicolon added on device if missing)."""
        msg = message.strip()
        if not msg:
            raise ValueError("empty Pd message")
        return self.command(f"MSG {msg}", timeout=5.0)

    def reset_device(self) -> None:
        self.command("RESET", timeout=2.0)

    def set_mode(self, mode: str) -> None:
        try:
            self.command(f"MODE {mode}", timeout=3.0)
        except (EspdDisconnected, TimeoutError):
            pass  # reboot disconnects CDC — expected

    def status(self) -> bytes:
        return self.command("STATUS", timeout=10.0)


def parse_status_info(line: bytes) -> dict[str, str]:
    m = _STATUS_INFO_RE.match(line.strip())
    if not m:
        raise RuntimeError(f"unexpected STATUS reply: {line.decode(errors='replace')}")
    return {
        "sdcard": m.group(1).decode(),
        "internal": m.group(2).decode(),
        "mode": m.group(3).decode(),
    }


def explain_status(info: dict[str, str]) -> str | None:
    """Human note when internal=no (not the same as USB MSC visible on the host)."""
    if info["internal"] == "yes":
        return None
    if info["mode"] == "normal":
        return (
            "note: /storage not mounted on device (USB MSC may still be visible on "
            "the host; espd_sync uses msc_sync to mount /storage for writes)"
        )
    if info["mode"] == "msc_sync":
        return "note: /storage not mounted on device yet"
    return "note: /storage not available on device"


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


def device_status(cdc: EspdCdc) -> dict[str, str]:
    if cdc.last_status and cdc.last_status.startswith(b"+OK STATUS"):
        return parse_status_info(cdc.last_status)
    return parse_status_info(cdc.status())


def sync_store_path(info: dict[str, str]) -> str:
    return "/sdcard" if info["sdcard"] == "yes" else "/storage"


def leave_msc_sync(cdc: EspdCdc, port: str) -> EspdCdc:
    """Reboot to normal mode so SD is used instead of exclusive /storage sync."""
    log_script("leaving msc_sync (device will reboot)")
    cdc.set_mode("NORMAL")
    cdc.close()
    time.sleep(0.5)
    wait_port_gone(port, timeout=25.0)
    time.sleep(2.0)
    log_script("waiting for CDC after reboot (up to 60s)…")
    return connect_cdc(port, ready_timeout=60.0)


def prepare_for_sync(cdc: EspdCdc, port: str) -> EspdCdc:
    """SD when a card is mounted; otherwise sync to internal flash (/storage)."""
    info = device_status(cdc)
    if info["sdcard"] == "yes":
        if info["mode"] == "msc_sync":
            log_script("SD card available — using /sdcard")
            cdc = leave_msc_sync(cdc, port)
        return cdc
    if info["internal"] != "yes" and info["mode"] != "msc_sync":
        log_script("no SD card — using /storage on device")
        note = explain_status(info)
        if note:
            log_script(note)
    return ensure_msc_sync_for_write(cdc, port)


def ensure_msc_sync_for_write(cdc: EspdCdc, port: str) -> EspdCdc:
    """Enter msc_sync and wait until /storage is mounted for PUT."""
    info = device_status(cdc)
    if info["mode"] == "msc_sync":
        if info["internal"] == "no":
            log_script("waiting for /storage mount on device…")
            for _ in range(30):
                if info["internal"] == "yes":
                    break
                time.sleep(0.5)
                info = parse_status_info(cdc.status())
        if info["internal"] != "yes":
            raise RuntimeError("/storage not mounted (msc_sync); check boot log on CDC")
        return cdc

    log_script("switching to msc_sync for flash write (device will reboot)")
    cdc.set_mode("MSC_SYNC")
    cdc.close()
    time.sleep(0.5)
    wait_port_gone(port, timeout=25.0)
    time.sleep(2.0)
    log_script("waiting for CDC after reboot (up to 60s)…")
    cdc = connect_cdc(port, ready_timeout=60.0)
    info = device_status(cdc)
    for attempt in range(60):
        if info["mode"] == "msc_sync" and info["internal"] == "yes":
            break
        if info["mode"] == "msc_sync" and info["internal"] == "no":
            if attempt == 0 or attempt % 10 == 0:
                log_script("waiting for /storage mount on device…")
        try:
            time.sleep(0.5)
            info = parse_status_info(cdc.status())
        except EspdDisconnected:
            log_script("CDC dropped during mode wait — reconnecting…")
            cdc.close()
            cdc = connect_cdc(port, ready_timeout=60.0)
            info = device_status(cdc)
    if info["mode"] != "msc_sync" or info["internal"] != "yes":
        raise RuntimeError(
            f"msc_sync failed: mode={info['mode']} internal={info['internal']}"
        )
    return cdc


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
            time.sleep(0.25)
            for _ in range(15):
                try:
                    last = cdc.status()
                    if last.startswith(b"+OK STATUS"):
                        log_script(f"connected ({port}): {last.decode(errors='replace')}")
                        connected = cdc
                        connected.last_status = last
                        cdc = None
                        return connected
                    if last.startswith(b"-ERR"):
                        time.sleep(0.25)
                        continue
                except EspdDisconnected as e:
                    last_err = str(e)
                    break
                time.sleep(0.4)
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
    while True:
        try:
            cdc = connect_cdc(port_pattern, ready_timeout=60.0)
            try:
                return prepare_for_sync(cdc, port_pattern)
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


def sync_files(cdc: EspdCdc, watch_dir: str, rels: list[str], port: str) -> EspdCdc:
    reload_needed = False
    reset_needed = False
    uploaded = 0
    skipped = 0
    t0 = time.time()

    def _sync_order(rel: str) -> tuple[int, str]:
        if rel == "main.pd":
            return (2, rel)
        if rel.endswith(".pd") or rel == "config.txt":
            return (1, rel)
        return (0, rel)  # samples/assets before patches

    for rel in sorted(rels, key=_sync_order):
        local = os.path.join(watch_dir, rel.replace("/", os.sep))
        _size, crc = local_file_hash(local)
        while True:
            try:
                sent = cdc.put_file(local, rel, crc)
            except (EspdDisconnected, TimeoutError) as e:
                kind = "timeout" if isinstance(e, TimeoutError) else "disconnect"
                log_script(f"{kind} during PUT {rel}; reconnecting…")
                cdc.close()
                try:
                    cdc = connect_and_prepare(port, exit_on_fail=True)
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
                    cdc.close()
                    cdc = connect_and_prepare(port, exit_on_fail=True)
                    continue
                raise
            if sent:
                uploaded += 1
                if rel == "config.txt":
                    reset_needed = True
                elif rel.endswith(".pd"):
                    reload_needed = True
            else:
                log_script(f"skip {rel} (unchanged)")
                skipped += 1
            break
    if reset_needed:
        log_script("RESET (config.txt applies on boot)")
        try:
            cdc.reset_device()
        except (EspdDisconnected, TimeoutError):
            pass
        try:
            cdc.close()
        except Exception:
            pass
        try:
            cdc = connect_and_prepare(port, exit_on_fail=False)
        except (TimeoutError, EspdDisconnected, RuntimeError) as e:
            log_script(f"reconnect after RESET: {e}")
            cdc = None
    elif reload_needed:
        log_script("RELOAD")
        try:
            cdc.reload()
        except (EspdDisconnected, TimeoutError):
            log_script("timeout/disconnect during RELOAD (patch may still reload on device)")
    log_script(
        f"sync done in {time.time() - t0:.2f}s "
        f"({uploaded} uploaded, {skipped} unchanged)"
    )
    return cdc


_PATCH_SUFFIXES = (".pd",)
_ASSET_SUFFIXES = (".wav", ".aiff", ".aif", ".flac", ".ogg", ".mp3", ".raw")


def _sync_name_ok(name: str, include_assets: bool) -> bool:
    if not name or name.startswith("."):
        return False
    if name == "config.txt":
        return True
    low = name.lower()
    if low.endswith(_PATCH_SUFFIXES):
        return True
    return include_assets and low.endswith(_ASSET_SUFFIXES)


def collect_files(root: str, include_assets: bool = False) -> dict[str, float]:
    out: dict[str, float] = {}
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if not _sync_name_ok(name, include_assets):
                continue
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            out[rel] = os.path.getmtime(full)
    return out


def main() -> int:
    _need_serial()
    ap = argparse.ArgumentParser(description="ESPD rapid sync over CDC")
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
    ap.add_argument("--reset", action="store_true", help="RESET device over CDC and exit")
    ap.add_argument("--no-color", action="store_true", help="disable ANSI colors")
    ap.add_argument(
        "--no-esp-log",
        action="store_true",
        help="hide ESP-IDF log lines (I/W/E tag: messages)",
    )
    ap.add_argument(
        "--no-reconnect",
        action="store_true",
        help="exit when USB disconnects instead of waiting for the port",
    )
    ap.add_argument(
        "--patches-only",
        action="store_true",
        help="initial/resync sync: only .pd and config.txt (default includes samples)",
    )
    ap.add_argument(
        "--no-initial-sync",
        action="store_true",
        help="skip the sync pass on connect (watch for saves only)",
    )
    ap.add_argument(
        "--resync-on-reconnect",
        action="store_true",
        help="after reconnect/reset, run the same PUT/sync pass again",
    )
    args = ap.parse_args()

    _out.color = not args.no_color
    _out.show_esp = not args.no_esp_log

    watch_dir = os.path.abspath(args.watch_dir)
    if not os.path.isdir(watch_dir):
        sys.exit(f"not a directory: {watch_dir}")
    confirm_watch_dir_has_main(watch_dir, yes=args.yes)

    try:
        port_pattern = resolve_port_pattern(args.port)
    except EspdDisconnected as e:
        sys.exit(f"port detect failed: {e}")

    cdc: EspdCdc | None = None
    try:
        include_assets = not args.patches_only

        def run_sync(label: str) -> None:
            nonlocal cdc
            rels = list(collect_files(watch_dir, include_assets))
            if not rels:
                log_script(f"{label}: nothing to send")
                return
            log_script(f"{label} ({len(rels)} files)")
            cdc = sync_files(cdc, watch_dir, rels, port_pattern)

        if args.reset:
            try:
                cdc = connect_cdc(port_pattern, ready_timeout=30.0)
                cdc.reset_device()
            except (TimeoutError, EspdDisconnected):
                pass
            cdc = None
            if args.no_reconnect:
                return 0
            cdc = connect_and_prepare(port_pattern)
            if not args.no_initial_sync:
                run_sync("sync after reset")
            return 0

        if args.status:
            cdc = connect_cdc(port_pattern, ready_timeout=30.0)
            note = explain_status(device_status(cdc))
            if note:
                log_script(note)
            return 0

        if args.pd_msg:
            cdc = connect_cdc(port_pattern, ready_timeout=30.0)
            line = cdc.send_pd(args.pd_msg)
            log_script(line.decode(errors="replace").strip())
            return 0

        cdc = connect_and_prepare(port_pattern)
        store = sync_store_path(device_status(cdc))

        if not args.no_initial_sync:
            run_sync("sync")

        mtimes = collect_files(watch_dir, include_assets=include_assets)
        watch_kind = "patches" if args.patches_only else "files"
        log_script(
            f"watching {watch_dir} ({len(mtimes)} {watch_kind}) — sync to {store}"
        )

        while True:
            if cdc is None or not cdc.alive:
                if args.no_reconnect:
                    log_script("device disconnected — exiting")
                    return 1
                cdc.close()
                cdc = None
                prev_store = store
                try:
                    cdc = connect_and_prepare(port_pattern)
                except (TimeoutError, EspdDisconnected, RuntimeError) as e:
                    log_script(str(e))
                    time.sleep(1.0)
                    continue
                store = sync_store_path(device_status(cdc))
                if store != prev_store:
                    log_script(f"sync target: {store}")
                    if not args.no_initial_sync:
                        run_sync("sync after reconnect")
                    mtimes = collect_files(watch_dir, include_assets=include_assets)
                elif args.resync_on_reconnect and not args.no_initial_sync:
                    run_sync("resync after reconnect")
                    mtimes = collect_files(watch_dir, include_assets=include_assets)

            time.sleep(0.2)
            try:
                now = collect_files(watch_dir, include_assets=include_assets)
                changed = [rel for rel in now if rel not in mtimes or now[rel] != mtimes[rel]]
                if not changed:
                    continue
                time.sleep(args.debounce)
                now2 = collect_files(watch_dir, include_assets=include_assets)
                changed = [
                    rel for rel in now2 if rel not in mtimes or now2[rel] != mtimes[rel]
                ]
                if not changed:
                    continue
                cdc = sync_files(cdc, watch_dir, changed, port_pattern)
                mtimes = now2
            except (EspdDisconnected, TimeoutError):
                log_script("timeout/disconnect during watch sync; staying in watch mode")
                try:
                    cdc.close()
                except Exception:
                    pass
                cdc = None
                continue
            except RuntimeError as e:
                log_script(f"sync error: {e}; staying in watch mode")
                try:
                    cdc.close()
                except Exception:
                    pass
                cdc = None
                continue

    except KeyboardInterrupt:
        log_script("stopped")
        return 0
    finally:
        if cdc:
            cdc.close()


if __name__ == "__main__":
    sys.exit(main())
