/*
 * [espdpulse <ch>] — hardware-timed GPIO pulse train (RMT).
 *
 *   rate <Hz>     pulse frequency
 *   duty <0..1>   fraction high (default 0.5)
 *   pulses <N>    N>0 finite burst, -1 continuous, 0 stop
 *
 * Channel index maps to pulse_pins= in config.txt. Timing is independent
 * of Pd's audio sample rate.
 */

#include "../pd/src/m_pd.h"
#include "espd_config.h"
#include "espd_pulse.h"

#ifdef ESPD_USE_PULSE

typedef struct _espd_pulse_obj {
    t_object x_obj;
    int ch;
} t_espd_pulse_obj;

static t_class *espd_pulse_class;

static void espd_pulse_rate(t_espd_pulse_obj *x, t_floatarg f)
{
    espd_pulse_set_rate(x->ch, f);
}

static void espd_pulse_duty(t_espd_pulse_obj *x, t_floatarg f)
{
    espd_pulse_set_duty(x->ch, f);
}

static void espd_pulse_pulses_msg(t_espd_pulse_obj *x, t_floatarg f)
{
    espd_pulse_pulses(x->ch, (int32_t)f);
}

static void *espd_pulse_new(t_floatarg fch)
{
    t_espd_pulse_obj *x = (t_espd_pulse_obj *)pd_new(espd_pulse_class);
    int ch = (int)fch;

    if (ch < 0 || ch >= ESPD_PULSE_MAX_CHANNELS) {
        pd_error(x, "espdpulse: channel must be 0..%d",
            ESPD_PULSE_MAX_CHANNELS - 1);
        ch = 0;
    }
    x->ch = ch;
    if (!espd_pulse_channel_ready(ch))
        pd_error(x, "espdpulse: channel %d not configured in config.txt", ch);
    return x;
}

void espd_pd_pulse_setup(void)
{
    espd_pulse_class = class_new(gensym("espdpulse"),
        (t_newmethod)espd_pulse_new, 0,
        sizeof(t_espd_pulse_obj), CLASS_DEFAULT, A_DEFFLOAT, 0);
    class_addmethod(espd_pulse_class, (t_method)espd_pulse_rate,
        gensym("rate"), A_FLOAT, 0);
    class_addmethod(espd_pulse_class, (t_method)espd_pulse_duty,
        gensym("duty"), A_FLOAT, 0);
    class_addmethod(espd_pulse_class, (t_method)espd_pulse_pulses_msg,
        gensym("pulses"), A_FLOAT, 0);
}

#else

void espd_pd_pulse_setup(void) {}

#endif /* ESPD_USE_PULSE */
