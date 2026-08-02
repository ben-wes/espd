/*
 * [espdnow] — ESP-NOW Pd object.
 *
 *   [espdnow]            ; RX on; no TX peer until 'peer <m0..m5>'
 *   [espdnow m0..m5]     ; RX on; TX peer set at creation
 *
 *   list ...             → FUDI → esp_now_send to the TX peer
 *   peer <m0..m5>        → set TX destination
 *   clear                → drop TX peer
 *   listen <m0..m5>      → RX filter: only that sender
 *   listen               → clear RX filter (hear everyone)
 *
 * Left outlet:  parsed FUDI from RX
 * Right outlet: meta —
 *   from <m0..m5>              (before each accepted RX)
 *   signal <rssi-dbm>          (before each accepted RX)
 *   send ok|fail|nopeer|toolong
 *   peer ok|fail <reason>|clear
 *   listen ok|fail <reason>|clear
 */

#include "../pd/src/m_pd.h"
#include "espd.h"
#include "espd_config.h"
#include "espd_config_file.h"
#include "espd_now.h"

#include <string.h>

#ifdef ESPD_USE_ESPNOW

#define ESPD_NOW_MAX_OBJECTS 8
#define ESPD_NOW_FUDI_MAX (ESPD_NOW_MAX_PAYLOAD + 2)

typedef struct _espdnow {
    t_object x_obj;
    uint8_t peer[6];
    int have_peer;
    uint8_t listen[6];
    int have_listen;
    t_outlet *x_out_data;
    t_outlet *x_out_status;
} t_espdnow;

static t_class *espdnow_class;
static t_espdnow *s_objects[ESPD_NOW_MAX_OBJECTS];
static int s_n_objects;
static t_binbuf *s_recv_bb;

static void status_out(t_espdnow *x, t_symbol *sel, int argc, t_atom *argv)
{
    outlet_anything(x->x_out_status, sel, argc, argv);
}

static int parse_mac_floats(int argc, t_atom *argv, uint8_t out[6])
{
    int i;

    if (argc != 6)
        return 0;
    for (i = 0; i < 6; i++)
        out[i] = (uint8_t)atom_getint(argv + i);
    return 1;
}

static int mac_is_broadcast(const uint8_t mac[6])
{
    return mac[0] == 0xff && mac[1] == 0xff && mac[2] == 0xff &&
        mac[3] == 0xff && mac[4] == 0xff && mac[5] == 0xff;
}

static esp_err_t peer_add_for_mac(const uint8_t mac[6])
{
    bool encrypt = !mac_is_broadcast(mac) && g_espd_cfg.espnow_have_pmk;
    return espd_now_peer_add(mac, encrypt);
}

static void emit_recv(t_espdnow *x, const uint8_t *data, int len)
{
    char buf[ESPD_NOW_FUDI_MAX + 1];
    int natom, msg, emsg;
    t_atom *at;

    if (len <= 0 || len > ESPD_NOW_MAX_PAYLOAD)
        return;
    memcpy(buf, data, len);
    buf[len] = '\0';

    binbuf_clear(s_recv_bb);
    binbuf_text(s_recv_bb, buf, len);
    natom = binbuf_getnatom(s_recv_bb);
    if (natom <= 0)
        return;
    at = binbuf_getvec(s_recv_bb);

    for (msg = 0; msg < natom;) {
        for (emsg = msg; emsg < natom && at[emsg].a_type != A_COMMA
             && at[emsg].a_type != A_SEMI; emsg++)
            ;
        if (emsg > msg) {
            if (at[msg].a_type == A_FLOAT) {
                if (emsg > msg + 1)
                    outlet_list(x->x_out_data, 0, emsg - msg, at + msg);
                else
                    outlet_float(x->x_out_data, at[msg].a_w.w_float);
            } else if (at[msg].a_type == A_SYMBOL) {
                outlet_anything(x->x_out_data, at[msg].a_w.w_symbol,
                    emsg - msg - 1, at + msg + 1);
            }
        }
        msg = emsg + 1;
    }
}

static void emit_status_send(t_espdnow *x, const espd_now_event_t *ev)
{
    t_atom ap;
    SETSYMBOL(&ap, gensym(ev->type == ESPD_NOW_EV_SEND_OK ? "ok" : "fail"));
    status_out(x, gensym("send"), 1, &ap);
}

static void emit_status_recv_meta(t_espdnow *x, const espd_now_event_t *ev)
{
    t_atom ap[6];
    int i;

    for (i = 0; i < 6; i++)
        SETFLOAT(ap + i, (t_float)ev->mac[i]);
    status_out(x, gensym("from"), 6, ap);
    SETFLOAT(ap, (t_float)ev->rssi);
    status_out(x, gensym("signal"), 1, ap);
}

static void espd_now_pd_handler(const espd_now_event_t *ev)
{
    int i;

    for (i = 0; i < s_n_objects; i++) {
        t_espdnow *x = s_objects[i];
        if (ev->type == ESPD_NOW_EV_RECV) {
            if (x->have_listen && memcmp(x->listen, ev->mac, 6) != 0)
                continue;
            emit_status_recv_meta(x, ev);
            emit_recv(x, ev->payload, ev->len);
        } else if (ev->owner == x) {
            emit_status_send(x, ev);
        }
    }
}

static int register_object(t_espdnow *x)
{
    if (s_n_objects >= ESPD_NOW_MAX_OBJECTS)
        return 0;
    s_objects[s_n_objects++] = x;
    return 1;
}

static void unregister_object(t_espdnow *x)
{
    int i, j;
    for (i = 0; i < s_n_objects; i++) {
        if (s_objects[i] == x) {
            for (j = i; j < s_n_objects - 1; j++)
                s_objects[j] = s_objects[j + 1];
            s_n_objects--;
            return;
        }
    }
}

static void espdnow_send(t_espdnow *x, t_symbol *s, int argc, t_atom *argv)
{
    t_binbuf *b;
    t_atom semi;
    char *buf = NULL;
    int length = 0;
    esp_err_t err;

    (void)s;
    if (!espd_now_ready()) {
        t_atom ap;
        SETSYMBOL(&ap, gensym("fail"));
        status_out(x, gensym("send"), 1, &ap);
        return;
    }
    if (!x->have_peer) {
        t_atom ap;
        SETSYMBOL(&ap, gensym("nopeer"));
        status_out(x, gensym("send"), 1, &ap);
        return;
    }

    b = binbuf_new();
    binbuf_add(b, argc, argv);
    SETSEMI(&semi);
    binbuf_add(b, 1, &semi);
    binbuf_gettext(b, &buf, &length);

    if (length <= 0 || length > ESPD_NOW_MAX_PAYLOAD) {
        t_atom ap;
        SETSYMBOL(&ap, gensym("toolong"));
        status_out(x, gensym("send"), 1, &ap);
    } else {
        err = espd_now_send(x, x->peer, (const uint8_t *)buf, length);
        if (err != ESP_OK) {
            t_atom ap;
            SETSYMBOL(&ap, gensym("fail"));
            status_out(x, gensym("send"), 1, &ap);
        }
    }

    if (buf && length > 0)
        freebytes(buf, length);
    binbuf_free(b);
}

static void espdnow_peer(t_espdnow *x, t_symbol *s, int argc, t_atom *argv)
{
    t_atom ap[2];
    uint8_t mac[6];

    (void)s;
    if (!parse_mac_floats(argc, argv, mac)) {
        SETSYMBOL(ap, gensym("fail"));
        SETSYMBOL(ap + 1, gensym("args"));
        status_out(x, gensym("peer"), 2, ap);
        return;
    }

    if (x->have_peer)
        espd_now_peer_del(x->peer);

    {
        esp_err_t err = peer_add_for_mac(mac);
        if (err != ESP_OK) {
            SETSYMBOL(ap, gensym("fail"));
            SETSYMBOL(ap + 1, gensym(esp_err_to_name(err)));
            status_out(x, gensym("peer"), 2, ap);
            return;
        }
    }
    memcpy(x->peer, mac, 6);
    x->have_peer = 1;
    SETSYMBOL(ap, gensym("ok"));
    status_out(x, gensym("peer"), 1, ap);
}

static void espdnow_clear(t_espdnow *x)
{
    t_atom ap;
    if (x->have_peer) {
        espd_now_peer_del(x->peer);
        x->have_peer = 0;
    }
    SETSYMBOL(&ap, gensym("clear"));
    status_out(x, gensym("peer"), 1, &ap);
}

static void espdnow_listen(t_espdnow *x, t_symbol *s, int argc, t_atom *argv)
{
    t_atom ap[2];
    uint8_t mac[6];

    (void)s;
    if (argc == 0) {
        x->have_listen = 0;
        SETSYMBOL(ap, gensym("clear"));
        status_out(x, gensym("listen"), 1, ap);
        return;
    }
    if (!parse_mac_floats(argc, argv, mac)) {
        SETSYMBOL(ap, gensym("fail"));
        SETSYMBOL(ap + 1, gensym("args"));
        status_out(x, gensym("listen"), 2, ap);
        return;
    }
    memcpy(x->listen, mac, 6);
    x->have_listen = 1;
    SETSYMBOL(ap, gensym("ok"));
    status_out(x, gensym("listen"), 1, ap);
}

static void *espdnow_new(t_symbol *s, int argc, t_atom *argv)
{
    t_espdnow *x = (t_espdnow *)pd_new(espdnow_class);
    uint8_t mac[6];

    (void)s;
    x->have_peer = 0;
    x->have_listen = 0;
    x->x_out_data = outlet_new(&x->x_obj, &s_anything);
    x->x_out_status = outlet_new(&x->x_obj, &s_anything);

    if (!register_object(x)) {
        pd_error(x, "espdnow: too many instances (max %d)",
            ESPD_NOW_MAX_OBJECTS);
        return x;
    }

    if (argc > 0) {
        if (!parse_mac_floats(argc, argv, mac)) {
            pd_error(x, "espdnow: creation arg must be 6 floats (MAC bytes)");
        } else if (peer_add_for_mac(mac) != ESP_OK) {
            pd_error(x, "espdnow: peer add failed at creation");
        } else {
            memcpy(x->peer, mac, 6);
            x->have_peer = 1;
        }
    }
    return x;
}

static void espdnow_free(t_espdnow *x)
{
    if (x->have_peer)
        espd_now_peer_del(x->peer);
    unregister_object(x);
}

void espd_pd_now_setup(void)
{
    espdnow_class = class_new(gensym("espdnow"),
        (t_newmethod)espdnow_new, (t_method)espdnow_free,
        sizeof(t_espdnow), CLASS_DEFAULT, A_GIMME, 0);
    class_addlist(espdnow_class, (t_method)espdnow_send);
    class_addmethod(espdnow_class, (t_method)espdnow_peer, gensym("peer"),
        A_GIMME, 0);
    class_addmethod(espdnow_class, (t_method)espdnow_clear, gensym("clear"),
        0);
    class_addmethod(espdnow_class, (t_method)espdnow_listen, gensym("listen"),
        A_GIMME, 0);

    if (!s_recv_bb)
        s_recv_bb = binbuf_new();
    espd_now_set_event_handler(espd_now_pd_handler);
}

#else /* !ESPD_USE_ESPNOW */

void espd_pd_now_setup(void) {}

#endif /* ESPD_USE_ESPNOW */
