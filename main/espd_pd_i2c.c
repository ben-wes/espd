/*
 * [espdi2c] — async I2C read/write.
 *
 *   [espdi2c <bus> <addr7>]
 *   | read <reg> <len>
 *   | write <reg> <b0> ...
 *   | write_raw <b0> ...
 *
 * Left outlet: byte list on successful read; bang on successful write/write_raw.
 * Right outlet (status): details only — sync busy|nobus|args, read|write fail …
 */

#include "../pd/src/m_pd.h"
#include "espd_config.h"
#include "espd_i2c.h"

#include <esp_err.h>
#include <string.h>

#ifdef ESPD_USE_I2C

typedef struct _espd_i2c_obj {
    t_object x_obj;
    int bus;
    uint8_t addr;
    int busy;
    t_outlet *x_out_data;
    t_outlet *x_out_status;
} t_espd_i2c_obj;

static t_class *espd_i2c_class;

static void espd_i2c_out_status_list(t_espd_i2c_obj *x, int argc, t_atom *argv)
{
    outlet_list(x->x_out_status, &s_list, argc, argv);
}

static void espd_i2c_out_sync(t_espd_i2c_obj *x, const char *reason)
{
    t_atom ap[2];
    SETSYMBOL(ap, gensym("sync"));
    SETSYMBOL(ap + 1, gensym(reason));
    espd_i2c_out_status_list(x, 2, ap);
}

static const char *espd_i2c_op_label(espd_i2c_op_t op)
{
    switch (op) {
    case ESPD_I2C_OP_READ:
        return "read";
    case ESPD_I2C_OP_WRITE:
        return "write";
    case ESPD_I2C_OP_WRITE_RAW:
        return "write_raw";
    default:
        return "xfer";
    }
}

static const char *espd_i2c_fail_reason(esp_err_t err)
{
    if (err == ESP_ERR_TIMEOUT)
        return "timeout";
    if (err == ESP_FAIL)
        return "nack";
    return "error";
}

static void espd_i2c_out_xfer(t_espd_i2c_obj *x, espd_i2c_op_t op,
    const char *result, const char *detail)
{
    t_atom ap[3];
    int n = 2;

    SETSYMBOL(ap, gensym(espd_i2c_op_label(op)));
    SETSYMBOL(ap + 1, gensym(result));
    if (detail) {
        SETSYMBOL(ap + 2, gensym(detail));
        n = 3;
    }
    espd_i2c_out_status_list(x, n, ap);
}

static void espd_i2c_out_bytes(t_espd_i2c_obj *x, const uint8_t *buf, int n)
{
    int i;
    t_atom ap[ESPD_I2C_MAX_READ];

    if (n <= 0)
        return;
    if (n > ESPD_I2C_MAX_READ)
        n = ESPD_I2C_MAX_READ;
    for (i = 0; i < n; i++)
        SETFLOAT(ap + i, (t_float)buf[i]);
    outlet_list(x->x_out_data, &s_list, n, ap);
}

static void espd_i2c_done(espd_i2c_job_t *job)
{
    t_espd_i2c_obj *x = (t_espd_i2c_obj *)job->owner;

    if (!x)
        return;

    x->busy = 0;

    if (job->result != ESP_OK) {
        espd_i2c_out_xfer(x, job->op, "fail",
            espd_i2c_fail_reason(job->result));
        return;
    }

    if (job->op == ESPD_I2C_OP_READ)
        espd_i2c_out_bytes(x, job->read_buf, job->read_len_actual);
    else
        outlet_bang(x->x_out_data);
}

static void espd_i2c_dispatch(t_espd_i2c_obj *x, espd_i2c_job_t *job)
{
    if (!espd_i2c_bus_ready(x->bus)) {
        espd_i2c_out_sync(x, "nobus");
        return;
    }
    if (x->busy) {
        espd_i2c_out_sync(x, "busy");
        return;
    }
    job->owner = x;
    job->bus = x->bus;
    job->addr = x->addr;
    if (!espd_i2c_submit(job)) {
        espd_i2c_out_sync(x, "busy");
        return;
    }
    x->busy = 1;
}

static void espd_i2c_read(t_espd_i2c_obj *x, t_symbol *s, int argc, t_atom *argv)
{
    espd_i2c_job_t job;

    (void)s;
    if (argc < 2) {
        espd_i2c_out_sync(x, "args");
        return;
    }
    memset(&job, 0, sizeof(job));
    job.op = ESPD_I2C_OP_READ;
    job.reg = (uint8_t)atom_getint(argv);
    job.read_len = (uint8_t)atom_getint(argv + 1);
    if (job.read_len == 0 || job.read_len > ESPD_I2C_MAX_READ) {
        espd_i2c_out_sync(x, "args");
        return;
    }
    espd_i2c_dispatch(x, &job);
}

static void espd_i2c_write(t_espd_i2c_obj *x, t_symbol *s, int argc, t_atom *argv)
{
    espd_i2c_job_t job;
    int i;

    (void)s;
    if (argc < 2) {
        espd_i2c_out_sync(x, "args");
        return;
    }
    memset(&job, 0, sizeof(job));
    job.op = ESPD_I2C_OP_WRITE;
    job.reg = (uint8_t)atom_getint(argv);
    job.write_len = (uint8_t)(argc - 1);
    if (job.write_len > ESPD_I2C_MAX_WRITE) {
        espd_i2c_out_sync(x, "args");
        return;
    }
    for (i = 0; i < job.write_len; i++)
        job.write_buf[i] = (uint8_t)atom_getint(argv + 1 + i);
    espd_i2c_dispatch(x, &job);
}

static void espd_i2c_write_raw(t_espd_i2c_obj *x, t_symbol *s, int argc,
    t_atom *argv)
{
    espd_i2c_job_t job;
    int i;

    (void)s;
    if (argc < 1) {
        espd_i2c_out_sync(x, "args");
        return;
    }
    memset(&job, 0, sizeof(job));
    job.op = ESPD_I2C_OP_WRITE_RAW;
    job.write_len = (uint8_t)argc;
    if (job.write_len > ESPD_I2C_MAX_WRITE) {
        espd_i2c_out_sync(x, "args");
        return;
    }
    for (i = 0; i < job.write_len; i++)
        job.write_buf[i] = (uint8_t)atom_getint(argv + i);
    espd_i2c_dispatch(x, &job);
}

static void espd_i2c_free(t_espd_i2c_obj *x)
{
    espd_i2c_release_device(x);
}

static void *espd_i2c_new(t_symbol *s, int argc, t_atom *argv)
{
    t_espd_i2c_obj *x = (t_espd_i2c_obj *)pd_new(espd_i2c_class);
    int bus = 0;
    int addr = 0x68;

    (void)s;
    if (argc >= 1)
        bus = atom_getint(argv);
    if (argc >= 2)
        addr = atom_getint(argv + 1);
    if (bus < 0 || bus >= ESPD_I2C_MAX_BUSES) {
        pd_error(x, "espdi2c: bus must be 0..%d", ESPD_I2C_MAX_BUSES - 1);
        bus = 0;
    }
    if (addr < 0 || addr > 127) {
        pd_error(x, "espdi2c: 7-bit address must be 0..127");
        addr = 0x68;
    }

    x->bus = bus;
    x->addr = (uint8_t)addr;
    x->busy = 0;
    x->x_out_data = outlet_new(&x->x_obj, &s_list);
    x->x_out_status = outlet_new(&x->x_obj, &s_list);
    if (!espd_i2c_bus_ready(bus))
        pd_error(x, "espdi2c: bus %d not configured in config.txt", bus);
    return x;
}

void espd_pd_i2c_setup(void)
{
    espd_i2c_class = class_new(gensym("espdi2c"),
        (t_newmethod)espd_i2c_new, (t_method)espd_i2c_free,
        sizeof(t_espd_i2c_obj), CLASS_DEFAULT, A_GIMME, 0);
    class_addmethod(espd_i2c_class, (t_method)espd_i2c_read, gensym("read"),
        A_GIMME, 0);
    class_addmethod(espd_i2c_class, (t_method)espd_i2c_write, gensym("write"),
        A_GIMME, 0);
    class_addmethod(espd_i2c_class, (t_method)espd_i2c_write_raw,
        gensym("write_raw"), A_GIMME, 0);
    espd_i2c_set_done_handler(espd_i2c_done);
}

#else

void espd_pd_i2c_setup(void) {}

#endif /* ESPD_USE_I2C */
