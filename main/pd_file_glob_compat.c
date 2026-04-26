#include "../pd/src/m_pd.h"
#include "espd.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

typedef struct _espd_file_glob {
    t_object x_obj;
    int x_verbose;
    t_outlet *x_dataout;
    t_outlet *x_infoout;
} t_espd_file_glob;

static t_class *espd_file_glob_class;
static char s_espd_pd_cwd[MAXPDSTRING];

static void espd_pd_cwd_init_once(void)
{
    if (!s_espd_pd_cwd[0]) {
        strncpy(s_espd_pd_cwd, ESPD_SDCARD_MOUNT, sizeof(s_espd_pd_cwd));
        s_espd_pd_cwd[sizeof(s_espd_pd_cwd) - 1] = '\0';
    }
}

const char *espd_pd_get_cwd(void)
{
    espd_pd_cwd_init_once();
    return s_espd_pd_cwd;
}

void espd_pd_set_cwd(const char *path)
{
    if (!path || !*path)
        return;
    strncpy(s_espd_pd_cwd, path, sizeof(s_espd_pd_cwd));
    s_espd_pd_cwd[sizeof(s_espd_pd_cwd) - 1] = '\0';
    {
        size_t n = strlen(s_espd_pd_cwd);
        while (n > 1 && s_espd_pd_cwd[n - 1] == '/') {
            s_espd_pd_cwd[n - 1] = '\0';
            n--;
        }
    }
}

static bool espd_glob_match_pattern(const char *pattern, const char *text)
{
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern)
                return true;
            while (*text) {
                if (espd_glob_match_pattern(pattern, text))
                    return true;
                text++;
            }
            return false;
        }
        if (*pattern == '?') {
            if (!*text)
                return false;
            pattern++;
            text++;
            continue;
        }
        if (*pattern != *text)
            return false;
        pattern++;
        text++;
    }
    return *text == '\0';
}

static void espd_glob_resolve_pattern(const char *in, char *out, size_t outsz)
{
    const char *base = espd_pd_get_cwd();

    if (!in || !*in) {
        out[0] = '\0';
        return;
    }

    if (in[0] == '/') {
        strncpy(out, in, outsz);
        out[outsz - 1] = '\0';
        return;
    }

    if (!strncmp(in, "./", 2))
        snprintf(out, outsz, "%s/%s", base, in + 2);
    else
        snprintf(out, outsz, "%s/%s", base, in);
    out[outsz - 1] = '\0';
}

static void espd_file_glob_emit_match(t_espd_file_glob *x, const char *path, int isdir)
{
    t_atom outv[2];
    SETSYMBOL(outv + 0, gensym(path));
    SETFLOAT(outv + 1, (t_float)isdir);
    outlet_list(x->x_dataout, gensym("list"), 2, outv);
}

static void espd_file_glob_symbol(t_espd_file_glob *x, t_symbol *spattern)
{
    const char *raw = spattern ? spattern->s_name : "";
    char pattern[MAXPDSTRING];
    char resolved[MAXPDSTRING];
    char dirbuf[MAXPDSTRING];
    const char *filepat;
    bool onlydirs = false;
    bool require_dot_match = false;
    int matches = 0;

    if (!raw || !*raw) {
        outlet_bang(x->x_infoout);
        return;
    }

    espd_glob_resolve_pattern(raw, resolved, sizeof(resolved));
    strncpy(pattern, resolved, sizeof(pattern));
    pattern[sizeof(pattern) - 1] = '\0';

    {
        size_t plen = strlen(pattern);
        if (plen > 0 && pattern[plen - 1] == '/') {
            onlydirs = true;
            while (plen > 0 && pattern[plen - 1] == '/') {
                pattern[plen - 1] = '\0';
                plen--;
            }
        }
    }

    {
        char *slash = strrchr(pattern, '/');
        if (!slash) {
            strncpy(dirbuf, ".", sizeof(dirbuf));
            dirbuf[sizeof(dirbuf) - 1] = '\0';
            filepat = pattern;
        } else {
            *slash = '\0';
            if (slash == pattern)
                strncpy(dirbuf, "/", sizeof(dirbuf));
            else
                strncpy(dirbuf, pattern, sizeof(dirbuf));
            dirbuf[sizeof(dirbuf) - 1] = '\0';
            filepat = slash + 1;
        }
    }

    require_dot_match = (filepat[0] == '.');

    if (!strchr(filepat, '*') && !strchr(filepat, '?')) {
        struct stat st;
        char full[MAXPDSTRING];
        if (!strcmp(dirbuf, "."))
            strncpy(full, filepat, sizeof(full));
        else
            snprintf(full, sizeof(full), "%s/%s", dirbuf, filepat);
        full[sizeof(full) - 1] = '\0';

        if (stat(full, &st) == 0) {
            int isdir = S_ISDIR(st.st_mode) ? 1 : 0;
            if (!onlydirs || isdir) {
                espd_file_glob_emit_match(x, full, isdir);
                matches++;
            }
        }
    } else {
        DIR *d = opendir(dirbuf);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                struct stat st;
                char full[MAXPDSTRING];
                int isdir;

                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                    continue;
                if (!require_dot_match && de->d_name[0] == '.')
                    continue;
                if (!espd_glob_match_pattern(filepat, de->d_name))
                    continue;

                if (!strcmp(dirbuf, "/"))
                    snprintf(full, sizeof(full), "/%s", de->d_name);
                else if (!strcmp(dirbuf, "."))
                    snprintf(full, sizeof(full), "%s", de->d_name);
                else
                    snprintf(full, sizeof(full), "%s/%s", dirbuf, de->d_name);
                full[sizeof(full) - 1] = '\0';

                if (stat(full, &st) != 0)
                    continue;
                isdir = S_ISDIR(st.st_mode) ? 1 : 0;
                if (onlydirs && !isdir)
                    continue;
                espd_file_glob_emit_match(x, full, isdir);
                matches++;
            }
            closedir(d);
        }
    }

    if (!matches)
        outlet_bang(x->x_infoout);
}

static void espd_file_glob_anything(t_espd_file_glob *x, t_symbol *s, int argc, t_atom *argv)
{
    (void)argc;
    (void)argv;
    espd_file_glob_symbol(x, s);
}

static void espd_file_glob_verbose(t_espd_file_glob *x, t_floatarg f)
{
    x->x_verbose = (f > 0.5f);
}

static void *espd_file_glob_new(t_symbol *s, int argc, t_atom *argv)
{
    t_espd_file_glob *x = (t_espd_file_glob *)pd_new(espd_file_glob_class);
    int saw_glob = 0;
    (void)s;
    x->x_verbose = 0;
    x->x_dataout = outlet_new(&x->x_obj, 0);
    x->x_infoout = outlet_new(&x->x_obj, 0);

    while (argc-- > 0) {
        if (argv->a_type == A_SYMBOL) {
            const char *sym = atom_getsymbol(argv)->s_name;
            if (!strcmp(sym, "glob"))
                saw_glob = 1;
            else if (!strcmp(sym, "-v"))
                x->x_verbose = 1;
        }
        argv++;
    }
    if (!saw_glob)
        pd_error(x, "file: only 'glob' mode is supported on this target");
    return x;
}

void espd_file_glob_setup(void)
{
    espd_file_glob_class = class_new(gensym("file"),
                                     (t_newmethod)espd_file_glob_new,
                                     0,
                                     sizeof(t_espd_file_glob),
                                     CLASS_DEFAULT,
                                     A_GIMME,
                                     0);
    class_addsymbol(espd_file_glob_class, (t_method)espd_file_glob_symbol);
    class_addanything(espd_file_glob_class, (t_method)espd_file_glob_anything);
    class_addmethod(espd_file_glob_class, (t_method)espd_file_glob_verbose,
                    gensym("verbose"), A_FLOAT, 0);
    class_sethelpsymbol(espd_file_glob_class, gensym("file"));
}
