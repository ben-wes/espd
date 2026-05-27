#!/usr/bin/env python3
"""
Watch a local Pure Data project folder and sync to ESP local storage over CDC.

Requires: pip install pyserial

Usage:
  python3 scripts/espd_sync.py -p PORT ./my_patch

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
_PING_INFO_RE = re.compile(
    rb"^\+OK PING target=(sd|msc) mounted=(yes|no) mode=(normal|msc_sync)$"
)

# USB CDC ignores baud; pace payload so TinyUSB/SD can drain without resetting the port.
_PUT_STREAM_BPS = 200000.0
_PUT_STREAM_CHUNK = 1024
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


def wait_for_port(pattern: str) -> str:
    while True:
        try:
            return resolve_port(pattern)
        except EspdDisconnected:
            time.sleep(0.5)


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
        self._dev_ready = threading.Event()
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
                if line.startswith(b"+OK dev ready"):
                    self._dev_ready.set()
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

    def wait_dev_ready(self, timeout: float) -> bool:
        """Wait for USB stack + espd_dev after port open (avoids PING during enumeration)."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self._check_alive()
            if self._dev_ready.is_set():
                return True
            time.sleep(0.05)
        return False

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
        probe_timeout = max(10.0, nbytes / 80000.0)
        done_timeout = max(60.0, nbytes / 8000.0)
        line = self.command(f"PUT {rel_path} {nbytes} {crc:08x}", timeout=probe_timeout)
        if line.startswith(b"+OK PUT skip"):
            return False
        if not line.startswith(b"+OK PUT ready"):
            if line.startswith(b"-ERR"):
                raise RuntimeError(line.decode(errors="replace"))
            raise RuntimeError(f"unexpected PUT reply: {line.decode(errors='replace')}")
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

    def reset_device(self) -> None:
        self.command("RESET", timeout=2.0)

    def set_mode(self, mode: str) -> None:
        try:
            self.command(f"MODE {mode}", timeout=3.0)
        except (EspdDisconnected, TimeoutError):
            pass  # reboot disconnects CDC — expected

    def ping(self) -> bytes:
        return self.command("PING", timeout=10.0)


def parse_ping_info(line: bytes) -> dict[str, str]:
    m = _PING_INFO_RE.match(line.strip())
    if not m:
        raise RuntimeError(f"unexpected PING reply: {line.decode(errors='replace')}")
    return {
        "target": m.group(1).decode(),
        "mounted": m.group(2).decode(),
        "mode": m.group(3).decode(),
    }


def ensure_msc_sync_for_write(cdc: EspdCdc, port: str) -> EspdCdc:
    """Switch to msc_sync only when about to PUT to internal flash."""
    info = parse_ping_info(cdc.ping())
    if info["target"] != "msc":
        return cdc
    if info["mode"] == "msc_sync":
        if info["mounted"] == "no":
            log_script("waiting for /storage mount…")
            for _ in range(30):
                time.sleep(0.5)
                info = parse_ping_info(cdc.ping())
                if info["mounted"] == "yes":
                    break
        if info["mounted"] != "yes":
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
    for attempt in range(60):
        try:
            info = parse_ping_info(cdc.ping())
        except EspdDisconnected:
            log_script("CDC dropped during mode wait — reconnecting…")
            cdc.close()
            cdc = connect_cdc(port, ready_timeout=60.0)
            continue
        if info["mode"] == "msc_sync" and info["mounted"] == "yes":
            break
        if info["mode"] == "msc_sync" and info["mounted"] == "no":
            if attempt == 0 or attempt % 10 == 0:
                log_script("waiting for /storage mount…")
        time.sleep(0.5)
    else:
        info = parse_ping_info(cdc.ping())
    if info["mode"] != "msc_sync" or info["mounted"] != "yes":
        raise RuntimeError(
            f"msc_sync failed: mode={info['mode']} mounted={info['mounted']}"
        )
    return cdc


def connect_cdc(port_pattern: str, ready_timeout: float = 8.0) -> EspdCdc:
    import serial.serialutil

    deadline = time.monotonic() + ready_timeout
    last_err = ""
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        cdc: EspdCdc | None = None
        try:
            port = wait_serial_ready(port_pattern, timeout=remaining)
            log_script(f"port: {port}")
            cdc = EspdCdc(port)
            if not cdc.wait_dev_ready(min(20.0, remaining)):
                last_err = "device USB not ready (+OK dev ready timeout)"
                continue
            time.sleep(0.25)
            for _ in range(15):
                try:
                    last = cdc.ping()
                    if last.startswith(b"+OK PING"):
                        log_script(f"connected ({port}): {last.decode(errors='replace')}")
                        connected = cdc
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
    """Connect over CDC (no mode switch — msc_sync only when syncing to flash)."""
    while True:
        try:
            return connect_cdc(port_pattern, ready_timeout=60.0)
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
    cdc = ensure_msc_sync_for_write(cdc, port)
    pd_changed = False
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
            log_script(f"PUT {rel}")
            try:
                sent = cdc.put_file(local, rel, crc)
            except EspdDisconnected:
                log_script(f"reconnect during PUT {rel}…")
                cdc.close()
                cdc = connect_cdc(port)
                cdc = ensure_msc_sync_for_write(cdc, port)
                continue
            except RuntimeError as e:
                if "crc mismatch" in str(e) or "crc exp" in str(e):
                    log_script(f"PUT verify failed for {rel}, retrying…")
                    continue
                raise
            if sent:
                uploaded += 1
                if rel == "main.pd" or rel.endswith(".pd"):
                    pd_changed = True
            else:
                log_script(f"skip {rel} (unchanged)")
                skipped += 1
            break
    if pd_changed:
        log_script("RELOAD")
        try:
            cdc.reload()
        except EspdDisconnected:
            log_script("disconnected during RELOAD (patch may still reload on device)")
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
    ap.add_argument("-p", "--port", required=True, help="CDC serial port path")
    ap.add_argument("watch_dir", help="Local folder to watch (contains main.pd)")
    ap.add_argument("--debounce", type=float, default=0.35, help="seconds after save")
    ap.add_argument("--ping", action="store_true", help="PING and exit")
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
    if not os.path.isfile(os.path.join(watch_dir, "main.pd")):
        log_script(f"warning: no main.pd in {watch_dir}")

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
            cdc = sync_files(cdc, watch_dir, rels, args.port)

        if args.reset:
            try:
                cdc = connect_cdc(args.port, ready_timeout=30.0)
                cdc.reset_device()
            except (TimeoutError, EspdDisconnected):
                pass
            cdc = None
            if args.no_reconnect:
                return 0
            cdc = connect_and_prepare(args.port)
            if not args.no_initial_sync:
                run_sync("sync after reset")
            return 0

        if args.ping:
            cdc = connect_cdc(args.port, ready_timeout=30.0)
            info = parse_ping_info(cdc.ping())
            log_script(
                f"device: mode={info['mode']} target={info['target']} mounted={info['mounted']}"
            )
            return 0

        cdc = connect_and_prepare(args.port)
        ping = parse_ping_info(cdc.ping())
        store = "/sdcard" if ping["target"] == "sd" else "/storage"

        if not args.no_initial_sync:
            run_sync("sync")

        mtimes = collect_files(watch_dir, include_assets=False)
        log_script(
            f"watching {watch_dir} ({len(mtimes)} patches) — sync to {store}"
        )

        while True:
            if not cdc.alive:
                if args.no_reconnect:
                    log_script("device disconnected — exiting")
                    return 1
                cdc.close()
                cdc = None
                try:
                    cdc = connect_and_prepare(args.port)
                except (TimeoutError, EspdDisconnected, RuntimeError) as e:
                    log_script(str(e))
                    time.sleep(1.0)
                    continue
                if args.resync_on_reconnect and not args.no_initial_sync:
                    run_sync("resync after reconnect")
                    mtimes = collect_files(watch_dir, include_assets=False)

            time.sleep(0.2)
            try:
                now = collect_files(watch_dir, include_assets=False)
                changed = [rel for rel in now if rel not in mtimes or now[rel] != mtimes[rel]]
                if not changed:
                    continue
                time.sleep(args.debounce)
                now2 = collect_files(watch_dir, include_assets=False)
                changed = [
                    rel for rel in now2 if rel not in mtimes or now2[rel] != mtimes[rel]
                ]
                if not changed:
                    continue
                cdc = sync_files(cdc, watch_dir, changed, args.port)
                mtimes = now2
            except EspdDisconnected:
                continue

    except KeyboardInterrupt:
        log_script("stopped")
        return 0
    finally:
        if cdc:
            cdc.close()


if __name__ == "__main__":
    sys.exit(main())
