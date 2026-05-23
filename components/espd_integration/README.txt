# Integrating a BSP with ESPD

Short reference — full guide: **docs/ADDING_A_BOARD.txt**

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
2. Select the board in menuconfig (**ESPD Configuration → Target board**), Save,
   then build. See **Switching boards** in **docs/ADDING_A_BOARD.txt**.
3. Override at configure time with **-DESPD_BSP_COMPONENT=...** if needed.
4. Each BSP registers in **Kconfig**: board choice, **ESPD_BSP_COMPONENT_NAME**
   default, **select** profile, and **sdkconfig.defaults** under **components/<name>/**.

## Switching boards

Configure first, then build (**idf.py** chains left to right):

```
idf.py menuconfig build flash monitor
```

Pick **Target board** → Save → exit; the chained **build** picks up **sdkconfig**,
merges that BSP's **sdkconfig.defaults**, and links **CONFIG_ESPD_BSP_COMPONENT_NAME**.
Full flow: **docs/ADDING_A_BOARD.txt** (*Switching boards*).

## Audio

- **Generic I2S** → **espd_audio_generic.c**
- **BSP codec** → **espd_audio_codec.c** (calls **bsp_audio_init()** etc.)

Select **ESPD Configuration → Audio backend** in menuconfig.

## Capacitive touch

SoC touch sensor → **espd/touch/N** (not BSP). Compile **ESPD_PD_USE_TOUCH0**;
activate with **touch_pins=** in **config.txt**. See **docs/ADDING_A_BOARD.txt**.

## Digital in / out (GPIO)

BSP buttons → **espd/din/0..** automatically when the BSP implements
**bsp_button_***. Extra GPIO digital inputs append at higher indices: compile
**ESPD_PD_USE_DIN0**, set **din_pins=** in **config.txt** (boot log lists the
full **espd/din/N** map). GPIO digital outputs: **ESPD_PD_USE_DOUT0** +
**dout_pins=** → **espd/dout/0..** (float ≥ 0.5 = high). PWM analog out is
separate: **ESPD_PD_USE_AOUT** + **aout_pins=** → **espd/aout/0..**.

## Moving BSP out of the ESPD tree

The reference **bsp_waveshare_s3** component is an example only. Production
projects should depend on it (or upstream esp-bsp) via Component Manager /
**idf_component.yml**, not fork it inside ESPD.
