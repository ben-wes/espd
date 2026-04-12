/* espdsp_osc_override.c - ESP32-friendly tilde overrides (float DSP, no double)
 *
 * Stock d_osc.c / d_osc.h use double + UNITBIT32 phase tricks; Xtensa has no
 * hardware double, so those inner loops are costly.
 *
 * After d_osc_setup(), we re-register osc~, phasor~, cos~, vcf~, tabosc4~.
 * Vanilla creators move to *~_aliased (see m_class.c). Patches keep the same
 * object names.
 *
 * Convention (small delta vs desktop Pd): our phasor~ outputs phase in [0,1)
 * per cycle; cos~ treats input as phase in cycles (0..1 = one cycle), same
 * scaling as feeding our phasor straight into cos~. Chains that relied on the
 * exact Hölderich bit layout may differ slightly; use *~_aliased if needed.
 */

#include "../pd/src/m_pd.h"
#include "../pd/src/m_imp.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef ESPDSP_BIGFLOAT
#define ESPDSP_BIGFLOAT 1.0e+19f
#endif

#define ESPDSP_OSC_TABSIZE 2048

static float *espdsp_costab;
static t_class *espdsp_osc_class;
static t_class *espdsp_phasor_class;
static t_class *espdsp_cos_class;
static t_class *espdsp_sigvcf_class;
static t_class *espdsp_tabosc4_class;

/* ----- shared cosine table (linear interp; tab[i+1] valid for i < TABSIZE) */

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
    /* match vanilla cardinal points */
    espdsp_costab[0] = espdsp_costab[ESPDSP_OSC_TABSIZE] = 1.f;
    espdsp_costab[ESPDSP_OSC_TABSIZE / 4] =
        espdsp_costab[3 * ESPDSP_OSC_TABSIZE / 4] = 0.f;
    espdsp_costab[ESPDSP_OSC_TABSIZE / 2] = -1.f;
}

static t_float espdsp_cos_lookup(float *tab, t_float p)
{
    int i;
    t_float frac, f1, f2;
    while (p >= (t_float)ESPDSP_OSC_TABSIZE)
        p -= (t_float)ESPDSP_OSC_TABSIZE;
    while (p < 0)
        p += (t_float)ESPDSP_OSC_TABSIZE;
    i = (int)p;
    if (i >= ESPDSP_OSC_TABSIZE)
        i %= ESPDSP_OSC_TABSIZE;
    if (i < 0)
        i = 0;
    frac = p - (t_float)i;
    f1 = tab[i];
    f2 = tab[i + 1];
    return f1 + frac * (f2 - f1);
}

/* -------------------------- osc~ ---------------------------------- */

typedef struct _espdsp_osc
{
    t_object x_obj;
    t_float x_phase; /* 0 .. ESPDSP_OSC_TABSIZE */
    t_float x_conv;
    t_float x_f;
} t_espdsp_osc;

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
        ph += *in++ * conv;
        while (ph >= (t_float)ESPDSP_OSC_TABSIZE)
            ph -= (t_float)ESPDSP_OSC_TABSIZE;
        while (ph < 0)
            ph += (t_float)ESPDSP_OSC_TABSIZE;
        *out++ = espdsp_cos_lookup(tab, ph);
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

/* -------------------------- phasor~ -------------------------------- */

typedef struct _espdsp_phasor
{
    t_object x_obj;
    t_float x_phase; /* 0 .. 1 */
    t_float x_conv;
    t_float x_f;
} t_espdsp_phasor;

static void *espdsp_phasor_new(t_floatarg f)
{
    t_espdsp_phasor *x = (t_espdsp_phasor *)pd_new(espdsp_phasor_class);
    x->x_f = f;
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_float, gensym("ft1"));
    x->x_phase = 0;
    x->x_conv = 0;
    outlet_new(&x->x_obj, gensym("signal"));
    return (x);
}

static t_int *espdsp_phasor_perform(t_int *w)
{
    t_espdsp_phasor *x = (t_espdsp_phasor *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *out = (t_sample *)(w[3]);
    int n = (int)(w[4]);
    t_float ph = x->x_phase;
    t_float conv = x->x_conv;

    while (n--)
    {
        *out++ = ph;
        ph += *in++ * conv;
        while (ph >= 1.f)
            ph -= 1.f;
        while (ph < 0.f)
            ph += 1.f;
    }
    x->x_phase = ph;
    return (w + 5);
}

static void espdsp_phasor_dsp(t_espdsp_phasor *x, t_signal **sp)
{
    x->x_conv = 1.f / (t_float)sp[0]->s_sr;
    dsp_add(espdsp_phasor_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, (t_int)sp[0]->s_n);
}

static void espdsp_phasor_ft1(t_espdsp_phasor *x, t_float f)
{
    t_float a = f - floorf(f);
    if (a < 0.f)
        a += 1.f;
    x->x_phase = a;
}

/* -------------------------- cos~ ----------------------------------- */

typedef struct _espdsp_cos
{
    t_object x_obj;
    t_float x_f;
} t_espdsp_cos;

static void *espdsp_cos_new(t_floatarg f)
{
    t_espdsp_cos *x = (t_espdsp_cos *)pd_new(espdsp_cos_class);
    outlet_new(&x->x_obj, gensym("signal"));
    x->x_f = f;
    return (x);
}

static t_int *espdsp_cos_perform(t_int *w)
{
    t_sample *in = (t_sample *)(w[1]);
    t_sample *out = (t_sample *)(w[2]);
    int n = (int)(w[3]);
    float *tab = espdsp_costab;

    if (!tab)
    {
        while (n--)
            *out++ = 0;
        return (w + 4);
    }

    while (n--)
    {
        t_float p = *in++ * (t_float)ESPDSP_OSC_TABSIZE;
        *out++ = espdsp_cos_lookup(tab, p);
    }
    return (w + 4);
}

static void espdsp_cos_dsp(t_espdsp_cos *x, t_signal **sp)
{
    (void)x;
    signal_setmultiout(&sp[1], sp[0]->s_nchans);
    dsp_add(espdsp_cos_perform, 3, sp[0]->s_vec, sp[1]->s_vec,
        (t_int)(sp[0]->s_length * sp[0]->s_nchans));
}

/* -------------------------- vcf~ ----------------------------------- */

typedef struct _espdsp_vcfctl
{
    t_float c_re;
    t_float c_im;
    t_float c_q;
    t_float c_isr;
} t_espdsp_vcfctl;

typedef struct _espdsp_sigvcf
{
    t_object x_obj;
    t_espdsp_vcfctl x_cspace;
    t_float x_f;
} t_espdsp_sigvcf;

static void *espdsp_sigvcf_new(t_floatarg q)
{
    t_espdsp_sigvcf *x = (t_espdsp_sigvcf *)pd_new(espdsp_sigvcf_class);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, gensym("float"), gensym("ft1"));
    outlet_new(&x->x_obj, gensym("signal"));
    outlet_new(&x->x_obj, gensym("signal"));
    x->x_cspace.c_re = 0;
    x->x_cspace.c_im = 0;
    x->x_cspace.c_q = q;
    x->x_cspace.c_isr = 0;
    x->x_f = 0;
    return (x);
}

static void espdsp_sigvcf_clear(t_espdsp_sigvcf *x)
{
    x->x_cspace.c_re = 0;
    x->x_cspace.c_im = 0;
}

static void espdsp_sigvcf_ft1(t_espdsp_sigvcf *x, t_float f)
{
    if (f < 0.f)
        f = 0.f;
    if (f > ESPDSP_BIGFLOAT)
        f = ESPDSP_BIGFLOAT;
    x->x_cspace.c_q = f;
}

static t_int *espdsp_sigvcf_perform(t_int *w)
{
    t_sample *in1 = (t_sample *)(w[1]);
    t_sample *in2 = (t_sample *)(w[2]);
    t_sample *out1 = (t_sample *)(w[3]);
    t_sample *out2 = (t_sample *)(w[4]);
    t_espdsp_vcfctl *c = (t_espdsp_vcfctl *)(w[5]);
    int n = (int)w[6];
    int i;
    t_float re = c->c_re, re2;
    t_float im = c->c_im;
    t_float q = c->c_q;
    t_float isr = c->c_isr;
    t_float qinv = (q > 0 ? 1.0f / q : 0.f);
    t_float ampcorrect = 2.f - 2.f / (q + 2.f);
    float *tab = espdsp_costab;
    t_float coefr, coefi;
    int ti, ti2;
    t_float p, frac, f1, f2;

    if (!tab)
    {
        for (i = 0; i < n; i++)
        {
            *out1++ = 0;
            *out2++ = 0;
        }
        return (w + 7);
    }

    for (i = 0; i < n; i++)
    {
        t_float cf, cfindx, r, oneminusr;
        cf = *in2++ * isr;
        if (cf < 0)
            cf = 0;
        cfindx = cf * (t_float)(ESPDSP_OSC_TABSIZE / 6.28318f);
        r = (qinv > 0 ? 1.f - cf * qinv : 0.f);
        if (r < 0)
            r = 0;
        oneminusr = 1.0f - r;

        p = cfindx;
        while (p >= (t_float)ESPDSP_OSC_TABSIZE)
            p -= (t_float)ESPDSP_OSC_TABSIZE;
        while (p < 0)
            p += (t_float)ESPDSP_OSC_TABSIZE;
        ti = (int)p;
        if (ti >= ESPDSP_OSC_TABSIZE)
            ti %= ESPDSP_OSC_TABSIZE;
        if (ti < 0)
            ti = 0;
        frac = p - (t_float)ti;
        f1 = tab[ti];
        f2 = tab[ti + 1];
        coefr = r * (f1 + frac * (f2 - f1));

        ti2 = (ti - (ESPDSP_OSC_TABSIZE / 4)) & (ESPDSP_OSC_TABSIZE - 1);
        f1 = tab[ti2];
        f2 = tab[ti2 + 1];
        coefi = r * (f1 + frac * (f2 - f1));

        f1 = *in1++;
        re2 = re;
        *out1++ = re = ampcorrect * oneminusr * f1 + coefr * re2 - coefi * im;
        *out2++ = im = coefi * re2 + coefr * im;
    }
    if (PD_BIGORSMALL(re))
        re = 0;
    if (PD_BIGORSMALL(im))
        im = 0;
    c->c_re = re;
    c->c_im = im;
    return (w + 7);
}

static void espdsp_sigvcf_dsp(t_espdsp_sigvcf *x, t_signal **sp)
{
    x->x_cspace.c_isr = 6.28318f / (t_float)sp[0]->s_sr;
    dsp_add(espdsp_sigvcf_perform, 6, sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec,
        sp[3]->s_vec, &x->x_cspace, (t_int)sp[0]->s_n);
}

/* -------------------------- tabosc4~ ------------------------------- */

typedef struct _espdsp_tabosc4
{
    t_object x_obj;
    t_float x_fnpoints;
    t_float x_finvnpoints;
    t_word *x_vec;
    t_symbol *x_arrayname;
    t_float x_f;
    t_float x_phase; /* normalized 0..1, like vanilla after each block */
    t_float x_conv;
} t_espdsp_tabosc4;

static void espdsp_tabosc4_set(t_espdsp_tabosc4 *x, t_symbol *s)
{
    t_garray *a;
    int npoints, pointsinarray;

    x->x_arrayname = s;
    if (!(a = (t_garray *)pd_findbyclass(x->x_arrayname, garray_class)))
    {
        if (*s->s_name)
            pd_error(x, "tabosc4~: %s: no such array", x->x_arrayname->s_name);
        x->x_vec = 0;
    }
    else if (!garray_getfloatwords(a, &pointsinarray, &x->x_vec))
    {
        pd_error(x, "%s: bad template for tabosc4~", x->x_arrayname->s_name);
        x->x_vec = 0;
    }
    else if ((npoints = pointsinarray - 3) != (1 << ilog2(pointsinarray - 3)))
    {
        pd_error(x, "%s: number of points (%d) not a power of 2 plus three",
            x->x_arrayname->s_name, pointsinarray);
        x->x_vec = 0;
    }
    else
    {
        x->x_fnpoints = (t_float)npoints;
        x->x_finvnpoints = 1.f / (t_float)npoints;
        garray_usedindsp(a);
    }
}

static void *espdsp_tabosc4_new(t_symbol *s)
{
    t_espdsp_tabosc4 *x = (t_espdsp_tabosc4 *)pd_new(espdsp_tabosc4_class);
    x->x_arrayname = s;
    x->x_vec = 0;
    x->x_fnpoints = 512.f;
    x->x_finvnpoints = (1.f / 512.f);
    outlet_new(&x->x_obj, gensym("signal"));
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_float, gensym("ft1"));
    x->x_f = 0;
    x->x_phase = 0;
    x->x_conv = 0;
    return (x);
}

static t_int *espdsp_tabosc4_perform(t_int *w)
{
    t_espdsp_tabosc4 *x = (t_espdsp_tabosc4 *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *out = (t_sample *)(w[3]);
    int n = (int)(w[4]);
    t_float fnpoints = x->x_fnpoints;
    int mask = (int)fnpoints - 1;
    t_float conv = fnpoints * x->x_conv;
    t_word *tab = x->x_vec;
    t_float ph = x->x_fnpoints * x->x_phase;
    t_sample frac, a, b, c, d, cminusb;
    t_word *addr;
    int idx;

    if (!tab)
        goto zero;

    while (ph >= fnpoints)
        ph -= fnpoints;
    while (ph < 0)
        ph += fnpoints;

    while (n--)
    {
        idx = (int)ph & mask;
        frac = ph - (t_float)idx;
        addr = tab + idx;
        a = addr[0].w_float;
        b = addr[1].w_float;
        c = addr[2].w_float;
        d = addr[3].w_float;
        cminusb = c - b;
        *out++ = b + frac * (
            cminusb - 0.1666667f * (1.f - frac) * (
                (d - a - 3.0f * cminusb) * frac + (d + 2.0f * a - 3.0f * b)
            )
        );
        ph += *in++ * conv;
        while (ph >= fnpoints)
            ph -= fnpoints;
        while (ph < 0)
            ph += fnpoints;
    }
    x->x_phase = ph * x->x_finvnpoints;
    return (w + 5);
 zero:
    while (n--)
        *out++ = 0;
    return (w + 5);
}

static void espdsp_tabosc4_ft1(t_espdsp_tabosc4 *x, t_float f)
{
    x->x_phase = f;
}

static void espdsp_tabosc4_dsp(t_espdsp_tabosc4 *x, t_signal **sp)
{
    x->x_conv = 1.f / (t_float)sp[0]->s_sr;
    espdsp_tabosc4_set(x, x->x_arrayname);
    dsp_add(espdsp_tabosc4_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, (t_int)sp[0]->s_n);
}

/* -------------------------- setup ---------------------------------- */

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

    espdsp_phasor_class = class_new(gensym("phasor~"),
        (t_newmethod)espdsp_phasor_new, 0, sizeof(t_espdsp_phasor), 0, A_DEFFLOAT, 0);
    CLASS_MAINSIGNALIN(espdsp_phasor_class, t_espdsp_phasor, x_f);
    class_addmethod(espdsp_phasor_class, (t_method)espdsp_phasor_dsp,
        gensym("dsp"), A_CANT, 0);
    class_addmethod(espdsp_phasor_class, (t_method)espdsp_phasor_ft1,
        gensym("ft1"), A_FLOAT, 0);

    espdsp_cos_class = class_new(gensym("cos~"), (t_newmethod)espdsp_cos_new, 0,
        sizeof(t_espdsp_cos), CLASS_MULTICHANNEL, A_DEFFLOAT, 0);
    CLASS_MAINSIGNALIN(espdsp_cos_class, t_espdsp_cos, x_f);
    class_addmethod(espdsp_cos_class, (t_method)espdsp_cos_dsp, gensym("dsp"),
        A_CANT, 0);

    espdsp_sigvcf_class = class_new(gensym("vcf~"),
        (t_newmethod)espdsp_sigvcf_new, 0, sizeof(t_espdsp_sigvcf), 0, A_DEFFLOAT, 0);
    CLASS_MAINSIGNALIN(espdsp_sigvcf_class, t_espdsp_sigvcf, x_f);
    class_addmethod(espdsp_sigvcf_class, (t_method)espdsp_sigvcf_dsp,
        gensym("dsp"), A_CANT, 0);
    class_addmethod(espdsp_sigvcf_class, (t_method)espdsp_sigvcf_ft1,
        gensym("ft1"), A_FLOAT, 0);
    class_addmethod(espdsp_sigvcf_class, (t_method)espdsp_sigvcf_clear,
        gensym("clear"), 0);

    espdsp_tabosc4_class = class_new(gensym("tabosc4~"),
        (t_newmethod)espdsp_tabosc4_new, 0, sizeof(t_espdsp_tabosc4), 0, A_DEFSYM, 0);
    CLASS_MAINSIGNALIN(espdsp_tabosc4_class, t_espdsp_tabosc4, x_f);
    class_addmethod(espdsp_tabosc4_class, (t_method)espdsp_tabosc4_dsp,
        gensym("dsp"), A_CANT, 0);
    class_addmethod(espdsp_tabosc4_class, (t_method)espdsp_tabosc4_set,
        gensym("set"), A_SYMBOL, 0);
    class_addmethod(espdsp_tabosc4_class, (t_method)espdsp_tabosc4_ft1,
        gensym("ft1"), A_FLOAT, 0);
}
