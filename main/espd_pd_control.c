/*
 * [espdcontrol] — espd platform queries.
 *
 *   ip  → list of four float octets 0..255, or symbol "noip"
 *   mac → list of six float bytes 0..255 (STA), or symbol "nomac"
 */

#include "../pd/src/m_pd.h"
#include "espd.h"
#include "espd_config.h"

#include <stdio.h>

typedef struct _espdcontrol {
    t_object x_obj;
    t_outlet *x_out;
} t_espdcontrol;

static t_class *espdcontrol_class;

#if defined(ESPD_USE_WIFI)
static void espdcontrol_ip(t_espdcontrol *x)
{
    int a, b, c, d;
    t_atom ap[4];

    if (!wifi_ipaddr[0] ||
        sscanf(wifi_ipaddr, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) {
        outlet_symbol(x->x_out, gensym("noip"));
        return;
    }
    SETFLOAT(ap + 0, (t_float)a);
    SETFLOAT(ap + 1, (t_float)b);
    SETFLOAT(ap + 2, (t_float)c);
    SETFLOAT(ap + 3, (t_float)d);
    outlet_list(x->x_out, 0, 4, ap);
}

static void espdcontrol_mac(t_espdcontrol *x)
{
    uint8_t m[6];
    t_atom ap[6];
    int i;

    if (!wifi_get_sta_mac(m)) {
        outlet_symbol(x->x_out, gensym("nomac"));
        return;
    }
    for (i = 0; i < 6; i++)
        SETFLOAT(&ap[i], (t_float)m[i]);
    outlet_list(x->x_out, 0, 6, ap);
}
#else
static void espdcontrol_ip(t_espdcontrol *x)
{
    outlet_symbol(x->x_out, gensym("noip"));
}

static void espdcontrol_mac(t_espdcontrol *x)
{
    outlet_symbol(x->x_out, gensym("nomac"));
}
#endif

static void *espdcontrol_new(void)
{
    t_espdcontrol *x = (t_espdcontrol *)pd_new(espdcontrol_class);
    x->x_out = outlet_new(&x->x_obj, &s_anything);
    return x;
}

void espd_pd_control_setup(void)
{
    espdcontrol_class = class_new(gensym("espdcontrol"),
        (t_newmethod)espdcontrol_new, 0, sizeof(t_espdcontrol),
        CLASS_DEFAULT, 0);
    class_addmethod(espdcontrol_class, (t_method)espdcontrol_ip,
        gensym("ip"), 0);
    class_addmethod(espdcontrol_class, (t_method)espdcontrol_mac,
        gensym("mac"), 0);
}
