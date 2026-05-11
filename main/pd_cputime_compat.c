#include "../pd/src/m_pd.h"
#include "espd.h"

static t_class *cputime_class;

typedef struct _cputime
{
    t_object x_obj;
} t_cputime;

static void cputime_bang(t_cputime *x)
{
    (void)x;
    espd_cputime_reset();
}

/* Same inlet layout as vanilla [cputime]. bang2 → 0.001 * espd_cputime_get()
 * (ms of summed wall µs per loop, excluding senddacs — not OS CPU like desktop). */
static void cputime_bang2(t_cputime *x)
{
    (void)x;
    outlet_float(x->x_obj.ob_outlet, 0.001f * (t_float)espd_cputime_get());
}

static void *cputime_new(void)
{
    t_cputime *x = (t_cputime *)pd_new(cputime_class);
    outlet_new(&x->x_obj, gensym("float"));
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, gensym("bang"), gensym("bang2"));
    cputime_bang(x);
    return x;
}

void espd_cputime_setup(void)
{
    cputime_class = class_new(gensym("cputime"), (t_newmethod)cputime_new, 0,
        sizeof(t_cputime), 0, 0);
    class_addbang(cputime_class, cputime_bang);
    class_addmethod(cputime_class, (t_method)cputime_bang2, gensym("bang2"), 0);
}
