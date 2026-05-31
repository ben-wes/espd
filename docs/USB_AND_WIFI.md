# USB OTG and Wi‑Fi (Waveshare ESP32-S3-AUDIO)

## Serial ports

| Port | Use |
|------|-----|
| `cu.usbmodem101` | Flash / esptool — close before app run |
| `cu.usbmodem1234561` | CDC logs + `espd_sync` |

Do not monitor `101` while the app uses `1234561`.

## `msc_sync` (RTC flag)

| `STATUS mode=` | Host USB disk | ESP `/storage` |
|----------------|---------------|----------------|
| **normal** | USB disk after Pd boot | APP for boot/config; then host; `PUT` reclaims APP briefly |
| **msc_sync** | Hidden | APP mount (legacy dev-sync reboot) |

- **`MODE MSC_SYNC`** — reboot into protected dev sync (`mode=msc_sync`).
- **Leave dev sync** — **reset or power-cycle** the board (clears the RTC flag). `MODE NORMAL` over CDC does the same without a button press.
- **`MODE NORMAL`** — optional CDC command (soft reboot, clears flag).

## Boot order

```
/storage (early VFS) → config.txt → Wi‑Fi PHY + CDC + MSC APP mount
→ SD optional → wifi_start_sta → wifi_wait_sta → pdmain → audio
```

Wi‑Fi credentials always come from `config.txt` before STA. USB never auto-mounts internal flash on the host at plug-in.

## Flash and monitor

```bash
idf.py -p /dev/cu.usbmodem1234561 monitor
```

Patch sync: **[DEV_SYNC.md](DEV_SYNC.md)**.
