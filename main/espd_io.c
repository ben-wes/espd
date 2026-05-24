/*
 * Generic ESPD I/O: Pd receivers for espd/din and espd/led, backed by
 * espd_board_* when a board profile provides hardware.
 */

#include "espd_io.h"
#include "espd_board.h"
#include "espd.h"
#include "espd_config.h"

#include "../pd/src/m_pd.h"

#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "espd_io";

#define ESPD_LED_MAX_RECEIVERS 16

typedef struct _espd_led_receiver {
    t_pd x_pd;
    int idx; /* -1 = master "espd/led", >=0 = per-LED "espd/led/N" */
} t_espd_led_receiver;

static t_class *espd_led_receiver_class;
static t_espd_led_receiver pd_led_master;
static t_espd_led_receiver pd_led_per[ESPD_LED_MAX_RECEIVERS];
static int pd_led_bound;

static int espd_led_clamp(t_float f)
{
    int v = (int)(f + 0.5f);
    if (v < 0)
        v = 0;
    else if (v > 255)
        v = 255;
#if ESPD_LED_MAX_BRIGHTNESS < 255
    v = (v * ESPD_LED_MAX_BRIGHTNESS) / 255;
    if (v > ESPD_LED_MAX_BRIGHTNESS)
        v = ESPD_LED_MAX_BRIGHTNESS;
#endif
    return v;
}

static void espd_led_receiver_list(t_espd_led_receiver *x, t_symbol *s,
    int argc, t_atom *argv)
{
    (void)s;
    if (x->idx >= 0) {
        if (argc < 3)
            return;
        uint8_t r = espd_led_clamp(atom_getfloat(argv));
        uint8_t g = espd_led_clamp(atom_getfloat(argv + 1));
        uint8_t b = espd_led_clamp(atom_getfloat(argv + 2));
        espd_board_led_set(x->idx, r, g, b);
        espd_board_led_mark_dirty();
        return;
    }
    if (argc == 3) {
        uint8_t r = espd_led_clamp(atom_getfloat(argv));
        uint8_t g = espd_led_clamp(atom_getfloat(argv + 1));
        uint8_t b = espd_led_clamp(atom_getfloat(argv + 2));
        espd_board_led_fill(r, g, b);
        espd_board_led_mark_dirty();
    } else if (argc >= 4) {
        int idx = (int)atom_getfloat(argv);
        uint8_t r = espd_led_clamp(atom_getfloat(argv + 1));
        uint8_t g = espd_led_clamp(atom_getfloat(argv + 2));
        uint8_t b = espd_led_clamp(atom_getfloat(argv + 3));
        espd_board_led_set(idx, r, g, b);
        espd_board_led_mark_dirty();
    }
}

static void espd_led_receiver_clear(t_espd_led_receiver *x)
{
    (void)x;
    espd_board_led_clear();
}

static void espd_io_bind_leds(void)
{
    int i;
    int count = espd_board_led_count();
    char name[16];

    if (count <= 0 || pd_led_bound)
        return;
    if (count > ESPD_LED_MAX_RECEIVERS)
        count = ESPD_LED_MAX_RECEIVERS;

    if (!espd_led_receiver_class) {
        espd_led_receiver_class = class_new(gensym("_espd_led_receiver"), 0, 0,
            sizeof(t_espd_led_receiver), CLASS_PD, 0);
        class_addlist(espd_led_receiver_class, (t_method)espd_led_receiver_list);
        class_addmethod(espd_led_receiver_class,
            (t_method)espd_led_receiver_clear, gensym("clear"), 0);
        class_addmethod(espd_led_receiver_class,
            (t_method)espd_led_receiver_clear, gensym("off"), 0);
    }

    pd_led_master.x_pd = espd_led_receiver_class;
    pd_led_master.idx = -1;
    pd_bind((t_pd *)&pd_led_master, gensym("espd/led"));

    for (i = 0; i < count; i++) {
        pd_led_per[i].x_pd = espd_led_receiver_class;
        pd_led_per[i].idx = i;
        snprintf(name, sizeof(name), "espd/led/%d", i);
        pd_bind((t_pd *)&pd_led_per[i], gensym(name));
    }
    pd_led_bound = 1;
    ESP_LOGI(TAG, "bound espd/led (%d pixels)", count);
}

void espd_din_changed(int idx, int pressed)
{
    char name[16];
    t_symbol *sym;
    t_pd *dest;

    snprintf(name, sizeof(name), "espd/din/%d", idx);
    sym = gensym(name);
    dest = sym ? sym->s_thing : NULL;
    if (dest)
        pd_float(dest, (t_float)(pressed ? 1 : 0));
}

void espd_io_poll(void)
{
    espd_board_poll();
#ifdef ESPD_USE_DIN
    espd_din_gpio_poll();
#endif
}

void espd_io_log_din_map(void)
{
    espd_din_log_map();
}

void espd_io_bind(void)
{
    espd_io_bind_leds();
}

esp_err_t espd_io_sdcard_mount(void)
{
    return espd_board_sdcard_mount();
}
