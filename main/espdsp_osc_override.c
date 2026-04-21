/* espdsp_osc_override.c - ESP32-friendly tilde overrides (float DSP, no double)
 *
 * After d_osc_setup() (and after d_array_setup() has registered tabread4~),
 * we re-register these names so embedded builds use the fast paths. Stock
 * classes remain available as *~_aliased (see m_class.c).
 *
 *   Overridden: osc~, cos~, vcf~, tabosc4~, tabread4~
 *
 *   phasor~ is left to stock d_osc.c: the float rewrite that lived here only
 *   produced zeros on-device; stock uses double in the loop but is correct.
 *
 * d_osc.c / d_osc.h use double + UNITBIT32 in hot loops; tabread4~ used
 * double for index sum. Xtensa has no hardware double.
 *
 * Convention (small delta vs desktop Pd): cos~ treats input as phase in cycles
 * (0..1 = one cycle). Chains that relied on the exact Hölderich bit layout may
 * differ; use *~_aliased.
 *
 * tabread4~ uses duplicated d_array.c arrayvec helpers (they are static there);
 * keep in sync when updating the pd submodule.
 */

#include "../pd/src/m_pd.h"
#include "../pd/src/m_imp.h"
#include "../pd/src/g_canvas.h"
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
static t_class *espdsp_cos_class;
static t_class *espdsp_sigvcf_class;
static t_class *espdsp_tabosc4_class;
static t_class *espdsp_tabread4_class;

#ifndef ESPDSP_MAX_PHASE
#define ESPDSP_MAX_PHASE 0x7fffffff
#endif

/* ----- tabread4~ : arrayvec copy (d_array.c helpers are file-static there) -- */

typedef struct _espdsp_dsparray
{
    t_symbol *d_symbol;
    t_gpointer d_gp;
    int d_phase;
    void *d_owner;
} t_espdsp_dsparray;

typedef struct _espdsp_arrayvec
{
    int v_n;
    t_espdsp_dsparray *v_vec;
} t_espdsp_arrayvec;

static int espdsp_dsparray_get_array(t_espdsp_dsparray *d, int *npoints,
    t_word **vec, int recover)
{
    t_garray *a;

    if (gpointer_check(&d->d_gp, 0))
    {
        *vec = (t_word *)d->d_gp.gp_stub->gs_un.gs_array->a_vec;
        *npoints = d->d_gp.gp_stub->gs_un.gs_array->a_n;
        return 1;
    }
    else if (recover || d->d_gp.gp_stub)
    {
        if (!(a = (t_garray *)pd_findbyclass(d->d_symbol, garray_class)))
        {
            if (d->d_owner && *d->d_symbol->s_name)
                pd_error(d->d_owner, "%s: no such array", d->d_symbol->s_name);
            gpointer_unset(&d->d_gp);
            return 0;
        }
        else if (!garray_getfloatwords(a, npoints, vec))
        {
            if (d->d_owner)
                pd_error(d->d_owner, "%s: bad template", d->d_symbol->s_name);
            gpointer_unset(&d->d_gp);
            return 0;
        }
        else
        {
            gpointer_setarray(&d->d_gp, garray_getarray(a), *vec);
            return 1;
        }
    }
    return 0;
}

static void espdsp_arrayvec_testvec(t_espdsp_arrayvec *v)
{
    int i, vecsize;
    t_word *vec;
    for (i = 0; i < v->v_n; i++)
    {
        if (*v->v_vec[i].d_symbol->s_name)
            espdsp_dsparray_get_array(&v->v_vec[i], &vecsize, &vec, 1);
    }
}

static void espdsp_arrayvec_set(t_espdsp_arrayvec *v, int argc, t_atom *argv)
{
    int i, oldsize = v->v_n;
    void *owner = v->v_vec[0].d_owner;
    if (!argc)
    {
        for (i = 0; i < v->v_n; i++)
        {
            gpointer_unset(&v->v_vec[i].d_gp);
            v->v_vec[i].d_symbol = &s_;
        }
        return;
    }
    for (i = 0; i < v->v_n; i++)
        gpointer_unset(&v->v_vec[i].d_gp);
    if (argc != oldsize)
    {
        v->v_vec = (t_espdsp_dsparray *)resizebytes(v->v_vec,
            oldsize * sizeof(*v->v_vec), argc * sizeof(*v->v_vec));
        v->v_n = argc;
        for (i = oldsize; i < v->v_n; i++)
        {
            gpointer_init(&v->v_vec[i].d_gp);
            v->v_vec[i].d_owner = owner;
            v->v_vec[i].d_phase = ESPDSP_MAX_PHASE;
        }
    }
    for (i = 0; i < v->v_n; i++)
    {
        if (argv[i].a_type != A_SYMBOL)
            pd_error(owner,
                "expected symbolic array name, got number instead"),
                v->v_vec[i].d_symbol = &s_;
        else
        {
            v->v_vec[i].d_phase = ESPDSP_MAX_PHASE;
            v->v_vec[i].d_symbol = argv[i].a_w.w_symbol;
        }
    }
    if (pd_getdspstate())
        espdsp_arrayvec_testvec(v);
}

static void espdsp_arrayvec_init(t_espdsp_arrayvec *v, void *x,
    int rawargc, t_atom *rawargv)
{
    int i, argc;
    t_atom a, *argv;
    if (rawargc == 0)
    {
        argc = 1;
        SETSYMBOL(&a, &s_);
        argv = &a;
    }
    else
        argc = rawargc, argv = rawargv;

    v->v_vec = (t_espdsp_dsparray *)getbytes(argc * sizeof(*v->v_vec));
    v->v_n = argc;
    for (i = 0; i < v->v_n; i++)
    {
        v->v_vec[i].d_owner = x;
        v->v_vec[i].d_phase = ESPDSP_MAX_PHASE;
        gpointer_init(&v->v_vec[i].d_gp);
    }
    espdsp_arrayvec_set(v, argc, argv);
}

static void espdsp_arrayvec_free(t_espdsp_arrayvec *v)
{
    int i;
    for (i = 0; i < v->v_n; i++)
        gpointer_unset(&v->v_vec[i].d_gp);
    freebytes(v->v_vec, v->v_n * sizeof(*v->v_vec));
}

typedef struct _espdsp_tabread4
{
    t_object x_obj;
    t_espdsp_arrayvec x_v;
    t_float x_f;
} t_espdsp_tabread4;

static void *espdsp_tabread4_new(t_symbol *s, int argc, t_atom *argv)
{
    t_espdsp_tabread4 *x = (t_espdsp_tabread4 *)pd_new(espdsp_tabread4_class);
    (void)s;
    espdsp_arrayvec_init(&x->x_v, x, argc, argv);
    signalinlet_new(&x->x_obj, 0);
    outlet_new(&x->x_obj, gensym("signal"));
    x->x_f = 0;
    return (x);
}

static t_int *espdsp_tabread4_perform(t_int *w)
{
    t_espdsp_dsparray *d = (t_espdsp_dsparray *)(w[1]);
    t_sample *in = (t_sample *)(w[2]);
    t_sample *onset = (t_sample *)(w[3]);
    t_sample *out = (t_sample *)(w[4]);
    int n = (int)(w[5]);
    int maxindex, i;
    t_word *buf, *wp;
    const t_sample one_over_six = 1.f / 6.f;

    if (!espdsp_dsparray_get_array(d, &maxindex, &buf, 0))
        goto zero;

    maxindex -= 3;
    if (maxindex < 1)
        goto zero;

    for (i = 0; i < n; i++)
    {
        t_float findex = *in++ + *onset++;
        int index = (int)findex;
        t_sample frac, a, b, c, d, cminusb;
        if (index < 1)
            index = 1, frac = 0;
        else if (index > maxindex)
            index = maxindex, frac = 1;
        else
            frac = findex - (t_float)index;
        wp = buf + index;
        a = wp[-1].w_float;
        b = wp[0].w_float;
        c = wp[1].w_float;
        d = wp[2].w_float;
        cminusb = c - b;
        *out++ = b + frac * (
            cminusb - one_over_six * ((t_sample)1.f - frac) * (
                (d - a - (t_sample)3.0f * cminusb) * frac +
                (d + a * (t_sample)2.0f - b * (t_sample)3.0f)
            )
        );
    }
    return (w + 6);
 zero:
    while (n--)
        *out++ = 0;

    return (w + 6);
}

static void espdsp_tabread4_set(t_espdsp_tabread4 *x, t_symbol *s,
    int argc, t_atom *argv)
{
    int nchans = x->x_v.v_n;
    (void)s;
    espdsp_arrayvec_set(&x->x_v, argc, argv);
    if (x->x_v.v_n != nchans)
        canvas_update_dsp();
}

static void espdsp_tabread4_dsp(t_espdsp_tabread4 *x, t_signal **sp)
{
    int i, length = sp[0]->s_length;
    int nchans = x->x_v.v_n;
    if (sp[0]->s_nchans > nchans)
        nchans = sp[0]->s_nchans;
    if (sp[1]->s_nchans > nchans)
        nchans = sp[1]->s_nchans;
    signal_setmultiout(&sp[2], nchans);
    espdsp_arrayvec_testvec(&x->x_v);
    for (i = 0; i < nchans; i++)
        dsp_add(espdsp_tabread4_perform, 5, x->x_v.v_vec + (i % x->x_v.v_n),
            sp[0]->s_vec + (i % sp[0]->s_nchans) * length,
            sp[1]->s_vec + (i % sp[1]->s_nchans) * length,
            sp[2]->s_vec + i * length, (t_int)length);
}

static void espdsp_tabread4_free(t_espdsp_tabread4 *x)
{
    espdsp_arrayvec_free(&x->x_v);
}

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

    espdsp_tabread4_class = class_new(gensym("tabread4~"),
        (t_newmethod)espdsp_tabread4_new, (t_method)espdsp_tabread4_free,
        sizeof(t_espdsp_tabread4), CLASS_MULTICHANNEL, A_GIMME, 0);
    CLASS_MAINSIGNALIN(espdsp_tabread4_class, t_espdsp_tabread4, x_f);
    class_addmethod(espdsp_tabread4_class, (t_method)espdsp_tabread4_dsp,
        gensym("dsp"), A_CANT, 0);
    class_addmethod(espdsp_tabread4_class, (t_method)espdsp_tabread4_set,
        gensym("set"), A_GIMME, 0);
}
