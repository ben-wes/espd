#include "../pd/src/m_pd.h"
#include "espd.h"

#include <stdio.h>
#include <string.h>

static char s_espd_pd_cwd[MAXPDSTRING];

static const char *espd_pd_get_cwd(void)
{
    if (!s_espd_pd_cwd[0]) {
        strncpy(s_espd_pd_cwd, ESPD_SDCARD_MOUNT, sizeof(s_espd_pd_cwd));
        s_espd_pd_cwd[sizeof(s_espd_pd_cwd) - 1] = '\0';
    }
    return s_espd_pd_cwd;
}

static void espd_pd_set_cwd(const char *path)
{
    size_t n;
    if (!path || !*path)
        return;
    strncpy(s_espd_pd_cwd, path, sizeof(s_espd_pd_cwd));
    s_espd_pd_cwd[sizeof(s_espd_pd_cwd) - 1] = '\0';
    n = strlen(s_espd_pd_cwd);
    while (n > 1 && s_espd_pd_cwd[n - 1] == '/') {
        s_espd_pd_cwd[n - 1] = '\0';
        n--;
    }
}

typedef struct _espd_pdcontrol {
    t_object x_obj;
    t_float x_level;
    t_outlet *x_out;
} t_espd_pdcontrol;

static t_class *espd_pdcontrol_class;

static void espd_pdcontrol_output_dir(t_espd_pdcontrol *x)
{
    outlet_symbol(x->x_out, gensym(espd_pd_get_cwd()));
}

static void espd_pdcontrol_dir(t_espd_pdcontrol *x, t_symbol *s, int argc, t_atom *argv)
{
    (void)s;
    if (argc > 0 && argv[0].a_type == A_FLOAT)
        x->x_level = atom_getfloat(argv + 0);
    espd_pdcontrol_output_dir(x);
}

static void espd_pdcontrol_chdir(t_espd_pdcontrol *x, t_symbol *s)
{
    const char *path = s ? s->s_name : "";
    char joined[MAXPDSTRING];

    if (!path || !*path)
        return;
    if (path[0] == '/')
        espd_pd_set_cwd(path);
    else {
        snprintf(joined, sizeof(joined), "%s/%s", espd_pd_get_cwd(), path);
        joined[sizeof(joined) - 1] = '\0';
        espd_pd_set_cwd(joined);
    }
    espd_pdcontrol_output_dir(x);
}

static void espd_pdcontrol_bang(t_espd_pdcontrol *x)
{
    espd_pdcontrol_output_dir(x);
}

#if defined(PD_USE_WIFI)
static void espd_pdcontrol_ip(t_espd_pdcontrol *x)
{
    int a, b, c, d, n;
    t_atom ap[4];

    (void)x;
    a = b = c = d = 0;
    n = 0;
    if (wifi_ipaddr[0])
        n = sscanf(wifi_ipaddr, "%d.%d.%d.%d", &a, &b, &c, &d);
    if (n != 4 || a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 ||
        d < 0 || d > 255) {
        a = b = c = d = 0;
    }
    SETFLOAT(&ap[0], (t_float)a);
    SETFLOAT(&ap[1], (t_float)b);
    SETFLOAT(&ap[2], (t_float)c);
    SETFLOAT(&ap[3], (t_float)d);
    outlet_list(x->x_out, 0, 4, ap);
}
#else
static void espd_pdcontrol_ip(t_espd_pdcontrol *x)
{
    t_atom ap[4];
    (void)x;
    SETFLOAT(&ap[0], 0);
    SETFLOAT(&ap[1], 0);
    SETFLOAT(&ap[2], 0);
    SETFLOAT(&ap[3], 0);
    outlet_list(x->x_out, 0, 4, ap);
}
#endif

static void *espd_pdcontrol_new(t_symbol *s, int argc, t_atom *argv)
{
    t_espd_pdcontrol *x = (t_espd_pdcontrol *)pd_new(espd_pdcontrol_class);
    (void)s;
    x->x_level = 0;
    if (argc > 0 && argv[0].a_type == A_FLOAT)
        x->x_level = atom_getfloat(argv + 0);
    floatinlet_new(&x->x_obj, &x->x_level);
    x->x_out = outlet_new(&x->x_obj, &s_anything);
    return x;
}

void espd_pdcontrol_setup(void)
{
    espd_pdcontrol_class = class_new(gensym("pdcontrol"),
                                     (t_newmethod)espd_pdcontrol_new,
                                     0,
                                     sizeof(t_espd_pdcontrol),
                                     CLASS_DEFAULT,
                                     A_GIMME,
                                     0);
    class_addbang(espd_pdcontrol_class, (t_method)espd_pdcontrol_bang);
    class_addmethod(espd_pdcontrol_class, (t_method)espd_pdcontrol_dir,
                    gensym("dir"), A_GIMME, 0);
    class_addmethod(espd_pdcontrol_class, (t_method)espd_pdcontrol_chdir,
                    gensym("chdir"), A_SYMBOL, 0);
    class_addmethod(espd_pdcontrol_class, (t_method)espd_pdcontrol_ip,
                    gensym("ip"), 0);
}
