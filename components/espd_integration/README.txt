# Integrating a BSP with ESPD

ESPD does not embed board-specific logic. Core firmware probes optional
**bsp_*** symbols; a BSP package you add to the project provides the ones
it supports.

## Upstream BSP (esp-bsp) — no ESPD code

Implement only the hardware drivers your board has. Match the optional
signatures in **espd_integration/include/bsp/bsp_io.h** and **bsp_audio.h**
(LED, buttons, SD, codec audio). No **espd_*** references.

## How ESPD discovers hardware

1. **espd_integration** ships weak stub **bsp_*** functions (no-op /
   `ESP_ERR_NOT_SUPPORTED`).
2. Your BSP component provides strong implementations for the peripherals it
   has.
3. **main/espd_board.c** calls every optional init at boot; failures other than
   `NOT_SUPPORTED` are logged.
4. **main/espd_io.c** binds Pd receivers when **bsp_led_count() > 0**.

No **espd_port.c** in the BSP.

## Project wiring

1. Add the BSP component to **components/** (or a managed dependency).
2. Select the board in **idf.py menuconfig → ESPD Configuration → Target board**.
   The BSP sets **CONFIG_ESPD_BSP_COMPONENT_NAME**; **main/CMakeLists.txt** links
   that component automatically.
3. Override at configure time with **-DESPD_BSP_COMPONENT=...** if needed.
4. Each BSP registers in **Kconfig**: board choice + **ESPD_BSP_COMPONENT_NAME**
   default + **sdkconfig.defaults** under **components/<name>/** (merged after
   menuconfig selects that board).

## Audio

- **Generic I2S** → **espd_audio_generic.c**
- **BSP codec** → **espd_audio_codec.c** (calls **bsp_audio_init()** etc.)

Select **ESPD Configuration → Audio backend** in menuconfig.

## Moving BSP out of the ESPD tree

The reference **bsp_waveshare_s3** component is an example only. Production
projects should depend on it (or upstream esp-bsp) via Component Manager /
**idf_component.yml**, not fork it inside ESPD.
