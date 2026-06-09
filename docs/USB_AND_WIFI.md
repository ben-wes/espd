# USB OTG and Wi‑Fi

## Serial ports

| Port | Use |
|------|-----|
| `cu.usbmodem101` | Flash / esptool — close before app run |
| `cu.usbmodem1234561` | CDC logs + `espd_sync` |

Do not monitor `101` while the app uses `1234561`.

## Internal flash and MSC

When internal flash is the only store (no SD card), boot may expose it as a USB
drive until the host ejects it once. After that, MSC is disabled and `/storage`
stays on the app side while Pd runs — dev sync is **CDC PUT** only, not a
host-mounted volume.

There is no separate “sync mode” RTC flag and no `MODE` / `MSC_SYNC` CDC
commands — use **`RESET`** (or power-cycle) if `STATUS` reports `internal=no`.

## Boot order

```
/storage (early VFS) → config.txt → wifi_prepare_phy → USB OTG (CDC/MSC)
→ SD optional → wifi_start_sta → board/audio init → wifi_wait_sta → pdmain
```

Wi‑Fi credentials always come from `config.txt` before STA.

## Flash and monitor

```bash
idf.py -p /dev/cu.usbmodem1234561 monitor
```

Patch sync: **[DEV_SYNC.md](DEV_SYNC.md)**.
