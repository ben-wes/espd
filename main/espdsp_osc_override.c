/* espdsp_osc_override.c - faster osc~ for ESP32 (float phase, no double in DSP)
 *
 * Stock Pd osc~ uses double + UNITBIT32 phase tricks in d_osc.h; on Xtensa
 * (ESP32 / ESP32-S3) double is software-emulated and dominates CPU cost.
 *
 * We re-register gensym("osc~") after d_osc_setup(); Pd then prefers this
 * creator and renames the vanilla one to "osc~_aliased" (see m_class.c).
 * Patches keep using [osc~] unchanged.
 */

#include "../pd/src/m_pd.h"
#include "../pd/src/m_imp.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define ESPDSP_OSC_TABSIZE 2048

static float *espdsp_costab;
static t_class *espdsp_osc_class;

typedef struct _espdsp_osc
{
    t_object x_obj;
    t_float x_phase; /* 0 .. ESPDSP_OSC_TABSIZE */
    t_float x_conv;
    t_float x_f;
} t_espdsp_osc;

static void espdsp_ensure_table(void)
{
    int i;
    if (espdsp_costab)
        return;
    espdsp_costab = (float *)getbytes(sizeof(float) * (ESPDSP_OSC_TABSIZE + 1));
    if (!espdsp_costab)
        return;
    for (i = 0; i <= ESPDSP_OSC_TABSIZE; i++)
        espdsp_costab[i] = cosf((t_float)(2. * M_PI * i / (double)ESPDSP_OSC_TABSIZE));
}

static void *espdsp_osc_new(t_floatarg f)
{
    t_espdsp_osc *x = (t_espdsp_osc *)pd_new(espdsp_osc_class);
    x->x_f = f;
    outlet_new(&x->x_obj, gensym("signal"));
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_float, gensym("ft1"));
    x->x_phase = 0;
    x->x_conv = 0;
    return (x);
}

static t_int *espdsp_osc_perform(t_int *w)
{
    t_espdsp_osc *x = (t_espdsp_osc *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *out = (t_sample *)(w[3]);
    int n = (int)(w[4]);
    float *tab = espdsp_costab;
    t_float ph = x->x_phase;
    t_float conv = x->x_conv;

    if (!tab)
    {
        while (n--)
            *out++ = 0;
        return (w + 5);
    }

    while (n--)
    {
        int i;
        t_float frac, f1, f2;
        ph += *in++ * conv;
        while (ph >= (t_float)ESPDSP_OSC_TABSIZE)
            ph -= (t_float)ESPDSP_OSC_TABSIZE;
        while (ph < 0)
            ph += (t_float)ESPDSP_OSC_TABSIZE;
        i = (int)ph;
        if (i >= ESPDSP_OSC_TABSIZE)
            i %= ESPDSP_OSC_TABSIZE;
        if (i < 0)
            i = 0;
        frac = ph - (t_float)i;
        f1 = tab[i];
        f2 = tab[i + 1]; /* tab[ESPDSP_OSC_TABSIZE] == tab[0] */
        *out++ = f1 + frac * (f2 - f1);
    }
    x->x_phase = ph;
    return (w + 5);
}

static void espdsp_osc_dsp(t_espdsp_osc *x, t_signal **sp)
{
    x->x_conv = (t_float)ESPDSP_OSC_TABSIZE / (t_float)sp[0]->s_sr;
    dsp_add(espdsp_osc_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, (t_int)sp[0]->s_n);
}

static void espdsp_osc_ft1(t_espdsp_osc *x, t_float f)
{
    x->x_phase = (t_float)ESPDSP_OSC_TABSIZE * f;
}

void espdsp_osc_override_setup(void)
{
    espdsp_ensure_table();
    espdsp_osc_class = class_new(gensym("osc~"), (t_newmethod)espdsp_osc_new, 0,
        sizeof(t_espdsp_osc), 0, A_DEFFLOAT, 0);
    CLASS_MAINSIGNALIN(espdsp_osc_class, t_espdsp_osc, x_f);
    class_addmethod(espdsp_osc_class, (t_method)espdsp_osc_dsp, gensym("dsp"),
        A_CANT, 0);
    class_addmethod(espdsp_osc_class, (t_method)espdsp_osc_ft1, gensym("ft1"),
        A_FLOAT, 0);
}
