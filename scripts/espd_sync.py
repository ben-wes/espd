#!/usr/bin/env python3
"""
Watch a local Pure Data project folder and sync to the ESP32 SD card over CDC.

Requires: pip install pyserial

Usage:
  python3 scripts/espd_sync.py -p '/dev/cu.usbmodem*' ~/my_patch

The ESP must have CONFIG_ESPD_DEV_CDC_SYNC and a microSD card mounted at /sdcard.
Internal flash MSC can stay mounted in Finder — rapid dev uses SD only.

Do not run idf.py monitor on the same port at the same time; this script tails logs.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import sys
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
    "pd": "\033[33m",
    "esp-i": "\033[90m",
    "esp-d": "\033[90m",
    "esp-v": "\033[90m",
    "esp-w": "\033[93m",
    "esp-e": "\033[31m",
    "script": "\033[2m",
    "misc": "",
}

# Device lines: +OK / -ERR (dev protocol). Pd: cpu:. ESP-IDF: I (123) tag: msg
_ESP_LOG_RE = re.compile(rb"^[IWEDV] \(\d+\) ")


def classify_line(line: bytes) -> str:
    if line.startswith(b"-ERR"):
        return "dev-err"
    if line.startswith(b"+"):
        return "dev-ok"
    if line.startswith(b"cpu:"):
        return "pd"
    if _ESP_LOG_RE.match(line):
        return "esp-" + line[:1].decode().lower()
    return "misc"


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
    _styled(sys.stderr, text, kind)


def log_device_line(line: bytes) -> None:
    kind = classify_line(line)
    if kind.startswith("esp-") and not _out.show_esp:
        return
    stream = sys.stdout if kind in ("pd", "misc") or kind.startswith("esp-") else sys.stderr
    if kind.startswith("esp-") or kind == "pd":
        _styled(stream, line.decode(errors="replace"), kind)
    else:
        _styled(stream, line.decode(errors="replace"), kind)


def _need_serial():
    try:
        import serial  # noqa: F401
        import serial.serialutil
    except ImportError:
        sys.exit("pip install pyserial")


class EspdDisconnected(Exception):
    """CDC port went away (reset, unplug, or sleep)."""


def resolve_port(pattern: str) -> str:
    """Expand shell-style globs; require exactly one CDC port."""
    if any(ch in pattern for ch in "*?[]"):
        matches = sorted(glob.glob(pattern))
        if not matches:
            raise EspdDisconnected(f"no serial port matches {pattern!r}")
        if len(matches) > 1:
            sys.exit(
                f"ambiguous port {pattern!r} ({len(matches)} devices):\n"
                + "\n".join(f"  {m}" for m in matches)
                + "\nPick one path explicitly."
            )
        return matches[0]
    if not os.path.exists(pattern):
        raise EspdDisconnected(f"serial port not found: {pattern!r}")
    return pattern


def wait_for_port(pattern: str, timeout: float | None) -> str:
    deadline = None if timeout is None else time.time() + timeout
    while True:
        try:
            return resolve_port(pattern)
        except EspdDisconnected:
            if deadline is not None and time.time() >= deadline:
                raise
            time.sleep(0.4)


class EspdCdc:
    def __init__(self, port: str):
        import serial

        self.port = port
        self._ser = serial.Serial()
        self._ser.port = port
        self._ser.baudrate = 115200
        self._ser.timeout = 0.05
        self._ser.open()
        if hasattr(self._ser, "exclusive"):
            self._ser.exclusive = True
        self._ser.dtr = False
        self._ser.rts = False
        time.sleep(0.05)
        self._ser.dtr = True
        self._ser.rts = True
        self._ser.reset_input_buffer()
        time.sleep(0.35)

        self._lock = threading.Lock()
        self._cmd_lock = threading.Lock()
        self._reply: bytes | None = None
        self._reply_event = threading.Event()
        self._proto_lines: list[bytes] = []
        self._stop = threading.Event()
        self._disconnected = threading.Event()
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
            except serial.serialutil.SerialException as e:
                if self._stop.is_set():
                    break
                self._mark_disconnected(str(e))
                break
            except OSError as e:
                if self._stop.is_set():
                    break
                self._mark_disconnected(str(e))
                break
            if not chunk:
                if self._disconnected.is_set():
                    break
                time.sleep(0.01)
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.strip(b"\r")
                if not line:
                    continue
                if line.startswith(b"+") or line.startswith(b"-ERR"):
                    with self._lock:
                        self._reply = line
                        self._proto_lines.append(line)
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

    def put_file(self, local_path: str, rel_path: str) -> None:
        data = open(local_path, "rb").read()
        rel_path = rel_path.replace(os.sep, "/")
        self.command(f"PUT {rel_path} {len(data)}", timeout=15.0)
        try:
            self._ser.write(data)
            self._ser.flush()
        except OSError as e:
            self._mark_disconnected(str(e))
            raise EspdDisconnected(str(e)) from e
        deadline = time.time() + max(60.0, len(data) / 20000.0)
        while time.time() < deadline:
            line = self._wait_reply(2.0)
            if line.startswith(b"+OK PUT done"):
                return
        raise TimeoutError(f"PUT {rel_path}: no done ack")

    def reload(self) -> None:
        self.command("RELOAD", timeout=30.0)

    def reset_device(self) -> None:
        """Ask firmware to esp_restart() after +OK RESET."""
        self.command("RESET", timeout=2.0)


def connect_cdc(port_pattern: str, debug: bool = False) -> EspdCdc:
    port = wait_for_port(port_pattern, timeout=None)
    log_script(f"port: {port}")
    cdc = EspdCdc(port)
    if cdc._reply_event.wait(2.0 if debug else 1.0):
        with cdc._lock:
            ready = cdc._reply or b""
            cdc._reply = None
        cdc._reply_event.clear()
        if ready:
            log_dev_rx(ready)

    last_err: Exception | None = None
    line = b""
    for attempt in range(5):
        try:
            if debug:
                log_script(f"PING attempt {attempt + 1}")
            line = cdc.command("PING", timeout=5.0)
            break
        except TimeoutError as e:
            last_err = e
            if not cdc.alive:
                raise EspdDisconnected("device gone during PING") from e
            time.sleep(0.25)
    else:
        raise last_err or TimeoutError("device reply timeout")

    log_script(f"connected ({port})")
    log_dev_rx(line)
    return cdc


def sync_files(cdc: EspdCdc, watch_dir: str, rels: list[str]) -> None:
    pd_changed = False
    t0 = time.time()
    for rel in sorted(rels):
        local = os.path.join(watch_dir, rel.replace("/", os.sep))
        log_script(f"PUT {rel}")
        cdc.put_file(local, rel)
        if rel == "main.pd" or rel.endswith(".pd"):
            pd_changed = True
    if pd_changed:
        log_script("RELOAD")
        cdc.reload()
    log_script(f"sync done in {time.time() - t0:.2f}s")


def collect_files(root: str) -> dict[str, float]:
    out: dict[str, float] = {}
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if not (name.endswith(".pd") or name == "config.txt"):
                continue
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            out[rel] = os.path.getmtime(full)
    return out


def main() -> int:
    _need_serial()
    ap = argparse.ArgumentParser(description="ESPD SD-card rapid sync over CDC")
    ap.add_argument("-p", "--port", required=True, help="CDC port (cu.usbmodem*)")
    ap.add_argument("watch_dir", help="Local folder to watch (contains main.pd)")
    ap.add_argument("--debounce", type=float, default=0.35, help="seconds after save")
    ap.add_argument("--ping", action="store_true", help="PING and exit")
    ap.add_argument("--reset", action="store_true", help="RESET device over CDC and exit")
    ap.add_argument("--debug", action="store_true", help="verbose connection diagnostics")
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
        "--reconnect-timeout",
        type=float,
        default=None,
        metavar="SEC",
        help="give up waiting for reconnect after SEC (default: wait forever)",
    )
    ap.add_argument(
        "--no-resync-on-reconnect",
        action="store_true",
        help="do not re-PUT all patch files after reconnect/reset",
    )
    args = ap.parse_args()

    _out.color = not args.no_color
    _out.show_esp = not args.no_esp_log

    watch_dir = os.path.abspath(args.watch_dir)
    if not os.path.isdir(watch_dir):
        sys.exit(f"not a directory: {watch_dir}")
    if not os.path.isfile(os.path.join(watch_dir, "main.pd")):
        log_script(f"warning: no main.pd in {watch_dir}")

    port_pattern = args.port
    cdc: EspdCdc | None = None

    try:
        try:
            cdc = connect_cdc(port_pattern, debug=args.debug)
        except (TimeoutError, EspdDisconnected) as e:
            log_script(f"connect failed: {e}")
            return 1

        if args.reset:
            try:
                cdc.reset_device()
            except EspdDisconnected:
                log_script("device reset (port closed)")
                return 0
            log_script("device resetting…")
            cdc.close()
            cdc = None
            if args.no_reconnect:
                return 0
            log_script("waiting for device to come back…")
            try:
                wait_for_port(port_pattern, args.reconnect_timeout)
                cdc = connect_cdc(port_pattern, debug=args.debug)
            except EspdDisconnected as e:
                log_script(str(e))
            return 1
            if not args.no_resync_on_reconnect:
                rels = list(collect_files(watch_dir))
                if rels:
                    log_script(f"resync after reset ({len(rels)} files)")
                    sync_files(cdc, watch_dir, rels)
            return 0

        if args.ping:
            return 0

        mtimes = collect_files(watch_dir)
        log_script(f"watching {watch_dir} ({len(mtimes)} files) — save in Pd to sync")

        while True:
            if cdc is None or not cdc.alive:
                if args.no_reconnect:
                    log_script("device disconnected — exiting")
                    return 1
                cdc = None
                log_script("waiting for device…")
                try:
                    wait_for_port(port_pattern, args.reconnect_timeout)
                    cdc = connect_cdc(port_pattern, debug=args.debug)
                except EspdDisconnected as e:
                    log_script(str(e))
                    return 1
                if not args.no_resync_on_reconnect:
                    rels = list(mtimes)
                    if rels:
                        log_script(f"resync after reconnect ({len(rels)} files)")
                        sync_files(cdc, watch_dir, rels)

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
                sync_files(cdc, watch_dir, changed)
                mtimes = now2
            except EspdDisconnected:
                if cdc:
                    cdc.close()
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
