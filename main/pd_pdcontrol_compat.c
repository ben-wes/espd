#include "../pd/src/m_pd.h"
#include "espd.h"

#include <stdio.h>
#include <string.h>

const char *espd_pd_get_cwd(void);
void espd_pd_set_cwd(const char *path);

typedef struct _espd_pdcontrol {
    t_object x_obj;
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
    (void)argc;
    (void)argv;
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

static void *espd_pdcontrol_new(void)
{
    t_espd_pdcontrol *x = (t_espd_pdcontrol *)pd_new(espd_pdcontrol_class);
    x->x_out = outlet_new(&x->x_obj, &s_symbol);
    return x;
}

void espd_pdcontrol_setup(void)
{
    espd_pdcontrol_class = class_new(gensym("pdcontrol"),
                                     (t_newmethod)espd_pdcontrol_new,
                                     0,
                                     sizeof(t_espd_pdcontrol),
                                     CLASS_DEFAULT,
                                     0);
    class_addbang(espd_pdcontrol_class, (t_method)espd_pdcontrol_bang);
    class_addmethod(espd_pdcontrol_class, (t_method)espd_pdcontrol_dir,
                    gensym("dir"), A_GIMME, 0);
    class_addmethod(espd_pdcontrol_class, (t_method)espd_pdcontrol_chdir,
                    gensym("chdir"), A_SYMBOL, 0);
}
