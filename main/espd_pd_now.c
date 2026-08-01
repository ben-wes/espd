/*
 * [espdnow] — ESP-NOW Pd object.
 *
 *   [espdnow]            ; RX always on; no peer; send → nopeer until 'peer <mac>'
 *   [espdnow <mac>]      ; RX always on; peer set; send works immediately
 *
 *   | anything on inlet → FUDI text → esp_now_send to the configured peer
 *   | peer <m0..m5>     → set peer MAC (6 floats); replaces current peer
 *   | clear             → drop the peer (RX-only again)
 *
 * Left outlet:  parsed FUDI message from RX (outlet_float/list/anything)
 * Right outlet: status (selector + args) —
 *   send ok | send fail | send nopeer | send toolong
 *   peer ok | peer fail <reason> | peer clear
 *   recv <m0..m5> <rssi>     (RSSI dBm; emitted before each RX data message)
 */

#include "../pd/src/m_pd.h"
#include "espd.h"
#include "espd_config.h"
#include "espd_config_file.h"
#include "espd_now.h"

#include <stdio.h>
#include <string.h>

#ifdef ESPD_USE_ESPNOW

#define ESPD_NOW_MAX_OBJECTS 8
#define ESPD_NOW_FUDI_MAX (ESPD_NOW_MAX_PAYLOAD + 2) /* room for ';' + NUL */

typedef struct _espdnow {
    t_object x_obj;
    uint8_t peer[6];
    int have_peer;
    t_outlet *x_out_data;
    t_outlet *x_out_status;
} t_espdnow;

static t_class *espdnow_class;
static t_espdnow *s_objects[ESPD_NOW_MAX_OBJECTS];
static int s_n_objects;
static t_binbuf *s_recv_bb;

/* ── Send: list / "send <args>" → FUDI → esp_now_send ──
 * Matches netsend: selector is not transmitted; only the args are shipped
 * (binbuf_add + SETSEMI + binbuf_gettext). The receiver reconstructs the
 * selector from the first atom. */

static void status_out(t_espdnow *x, t_symbol *sel, int argc, t_atom *argv)
{
    outlet_anything(x->x_out_status, sel, argc, argv);
}

/* ── FUDI parsing (text → Pd outlet message) ── */

static void emit_recv(t_espdnow *x, const uint8_t *data, int len)
{
    char buf[ESPD_NOW_FUDI_MAX + 1];
    int natom, msg, emsg;
    t_atom *at;

    if (len <= 0 || len > ESPD_NOW_MAX_PAYLOAD)
        return;
    /* TX already appends ';' (netsend-style); do not add another. */
    memcpy(buf, data, len);
    buf[len] = '\0';

    binbuf_clear(s_recv_bb);
    binbuf_text(s_recv_bb, buf, len);
    natom = binbuf_getnatom(s_recv_bb);
    if (natom <= 0)
        return;
    at = binbuf_getvec(s_recv_bb);

    /* Same framing as netsend_read: one outlet message per ; / , chunk. */
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
    t_atom ap[7];
    int i;

    for (i = 0; i < 6; i++)
        SETFLOAT(ap + i, (t_float)ev->mac[i]);
    SETFLOAT(ap + 6, (t_float)ev->rssi);
    status_out(x, gensym("recv"), 7, ap);
}

/* ── Core event handler (called on Pd thread via espd_now_poll) ── */

static void espd_now_pd_handler(const espd_now_event_t *ev)
{
    int i;

    for (i = 0; i < s_n_objects; i++) {
        t_espdnow *x = s_objects[i];
        if (ev->type == ESPD_NOW_EV_RECV) {
            emit_status_recv_meta(x, ev);
            emit_recv(x, ev->payload, ev->len);
        } else if (ev->owner == x) {
            emit_status_send(x, ev);
        }
    }
}

/* ── Object registration ── */

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

/* ── Inlet: anything → FUDI → esp_now_send ── */

static void espdnow_send(t_espdnow *x, t_symbol *s, int argc, t_atom *argv)
{
    t_binbuf *b;
    t_atom semi;
    char *buf = NULL;
    int length = 0;
    esp_err_t err;

    (void)s;  /* selector not transmitted — see netsend */
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

/* ── peer <m0..m5> ── */

static void espdnow_peer(t_espdnow *x, t_symbol *s, int argc, t_atom *argv)
{
    t_atom ap[3];
    uint8_t mac[6];
    int i;

    (void)s;
    if (argc != 6) {
        SETSYMBOL(ap, gensym("fail"));
        SETSYMBOL(ap + 1, gensym("args"));
        status_out(x, gensym("peer"), 2, ap);
        return;
    }
    for (i = 0; i < 6; i++) {
        int v = (int)atom_getfloat(argv + i);
        if (v < 0 || v > 255) {
            SETSYMBOL(ap, gensym("fail"));
            SETSYMBOL(ap + 1, gensym("range"));
            status_out(x, gensym("peer"), 2, ap);
            return;
        }
        mac[i] = (uint8_t)v;
    }

    /* If we already had a peer, remove it from the ESP-NOW table first. */
    if (x->have_peer)
        espd_now_peer_del(x->peer);

    /* Broadcast (ff:ff:ff:ff:ff:ff) is always unencrypted; otherwise encrypt
     * if a PMK is configured. */
    bool is_bcast = (mac[0] == 0xff && mac[1] == 0xff && mac[2] == 0xff &&
        mac[3] == 0xff && mac[4] == 0xff && mac[5] == 0xff);
    bool encrypt = !is_bcast &&
        (g_espd_cfg.espnow_have_pmk != 0);

    esp_err_t err = espd_now_peer_add(mac, encrypt);
    if (err != ESP_OK) {
        SETSYMBOL(ap, gensym("fail"));
        SETSYMBOL(ap + 1, gensym(esp_err_to_name(err)));
        status_out(x, gensym("peer"), 2, ap);
        return;
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

/* ── creation / destruction ── */

static int parse_mac_arg(t_symbol *s, int argc, t_atom *argv, uint8_t out[6])
{
    int i;
    (void)s;
    if (argc != 6)
        return 0;
    for (i = 0; i < 6; i++) {
        int v = (int)atom_getfloat(argv + i);
        if (v < 0 || v > 255)
            return 0;
        out[i] = (uint8_t)v;
    }
    return 1;
}

static void *espdnow_new(t_symbol *s, int argc, t_atom *argv)
{
    t_espdnow *x = (t_espdnow *)pd_new(espdnow_class);
    uint8_t mac[6];

    x->have_peer = 0;
    x->x_out_data = outlet_new(&x->x_obj, &s_anything);
    x->x_out_status = outlet_new(&x->x_obj, &s_list);

    if (!register_object(x)) {
        pd_error(x, "espdnow: too many instances (max %d)",
            ESPD_NOW_MAX_OBJECTS);
        return x;
    }

    if (argc >= 6 && parse_mac_arg(s, argc, argv, mac)) {
        bool is_bcast = (mac[0] == 0xff && mac[1] == 0xff && mac[2] == 0xff &&
            mac[3] == 0xff && mac[4] == 0xff && mac[5] == 0xff);
        bool encrypt = !is_bcast && (g_espd_cfg.espnow_have_pmk != 0);
        if (espd_now_peer_add(mac, encrypt) == ESP_OK) {
            memcpy(x->peer, mac, 6);
            x->have_peer = 1;
        } else {
            pd_error(x, "espdnow: peer add failed at creation");
        }
    } else if (argc > 0) {
        pd_error(x, "espdnow: creation arg must be 6 floats (MAC bytes)");
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

    if (!s_recv_bb)
        s_recv_bb = binbuf_new();
    espd_now_set_event_handler(espd_now_pd_handler);
}

#else /* !ESPD_USE_ESPNOW */

void espd_pd_now_setup(void) {}

#endif /* ESPD_USE_ESPNOW */
