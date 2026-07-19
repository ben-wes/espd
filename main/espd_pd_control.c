/*
 * [espdcontrol] — espd platform queries.
 *
 *   ip → list of four float octets 0..255, or symbol "noip"
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
    int a, b, c, d, n;
    t_atom ap[4];

    a = b = c = d = 0;
    n = 0;
    if (wifi_ipaddr[0])
        n = sscanf(wifi_ipaddr, "%d.%d.%d.%d", &a, &b, &c, &d);
    if (n != 4 || a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 ||
        d < 0 || d > 255) {
        outlet_symbol(x->x_out, gensym("noip"));
        return;
    }
    SETFLOAT(&ap[0], (t_float)a);
    SETFLOAT(&ap[1], (t_float)b);
    SETFLOAT(&ap[2], (t_float)c);
    SETFLOAT(&ap[3], (t_float)d);
    outlet_list(x->x_out, 0, 4, ap);
}
#else
static void espdcontrol_ip(t_espdcontrol *x)
{
    outlet_symbol(x->x_out, gensym("noip"));
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
}
