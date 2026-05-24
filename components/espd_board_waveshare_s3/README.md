# Waveshare ESP32-S3-AUDIO — ESPD board plugin

Self-contained integration for the Waveshare kit via [esp-bsp](https://github.com/ben-wes/esp-bsp/tree/waveshare-bsp).

**Naming convention:** folder `espd_board_waveshare_s3` ↔ Kconfig `ESPD_BOARD_WAVESHARE_S3`.
Root **CMakeLists.txt** and **espd_boards** discover this plugin automatically — no
Waveshare-specific branches elsewhere.

| File | Role |
|------|------|
| **Kconfig.board** | menuconfig board entry + **imply** for ESPD feature flags |
| **idf_component.yml** | `path:` dep on `espd_integration` + git dep on upstream esp-bsp |
| **sdkconfig.defaults** | ESPD profile: features, board hardware, Pd runtime tuning (see section comments) |
| **waveshare_*.c** | kit glue (**bsp_io.h**, **espd_bsp_audio_hw_init** → esp-bsp) |
| **CMakeLists.txt** | registers sources only — no `REQUIRES` (all deps via `idf_component.yml`) |

**Fresh configure:** pick **Waveshare ESP32-S3-AUDIO** in menuconfig → save →
**build** (IDF profile defaults apply on that reconfigure). For PSRAM and other
IDF options on the *first* menuconfig, append **CONFIG_ESPD_BOARD_WAVESHARE_S3=y**
to **sdkconfig.defaults.esp32s3** (see **sdkconfig.defaults.esp32s3.example**).

To add another board: copy this folder as **espd_board_mykit/** (name must match
the Kconfig symbol suffix). See [docs/ADDING_A_BOARD.md](../../docs/ADDING_A_BOARD.md).
