#include "espd.h"
#include "espd_storage.h"

#include <dirent.h>
#include <errno.h>
#include <glob.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int espd_glob_match(const char *pattern, const char *text)
{
    while (*pattern) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern)
                return 1;
            while (*text) {
                if (espd_glob_match(pattern, text))
                    return 1;
                text++;
            }
            return 0;
        }
        if (*pattern == '?') {
            if (!*text)
                return 0;
            pattern++;
            text++;
            continue;
        }
        if (*pattern != *text)
            return 0;
        pattern++;
        text++;
    }
    return *text == '\0';
}

static int espd_glob_add(glob_t *pglob, const char *path)
{
    char *copy = strdup(path);
    char **newv;
    if (!copy)
        return GLOB_NOSPACE;
    newv = (char **)realloc(pglob->gl_pathv, sizeof(char *) * (pglob->gl_pathc + 2));
    if (!newv) {
        free(copy);
        return GLOB_NOSPACE;
    }
    pglob->gl_pathv = newv;
    pglob->gl_pathv[pglob->gl_pathc++] = copy;
    pglob->gl_pathv[pglob->gl_pathc] = NULL;
    return 0;
}

static void espd_unbash_inplace(char *s)
{
    char *r = s;
    char *w = s;
    while (*r) {
        if (*r == '\\' && r[1])
            r++;
        *w++ = *r++;
    }
    *w = '\0';
}

int glob(const char *pattern, int flags,
         int (*errfunc)(const char *epath, int eerrno), glob_t *pglob)
{
    char resolved[1200];
    const char *use_pattern = pattern;
    char patbuf[1024];
    char dirbuf[1024];
    const char *filepat;
    char *slash;
    int onlydirs = 0;
    DIR *d;
    struct dirent *de;
    int rc = 0;
    (void)flags;
    (void)errfunc;

    if (!pglob || !pattern)
        return GLOB_NOMATCH;

    if (pattern[0] != '/') {
        const char *root = espd_storage_main_pd_mount_dir();
        if (!root || !root[0])
            root = ESPD_SDCARD_MOUNT;
        if (!strncmp(pattern, "./", 2))
            snprintf(resolved, sizeof(resolved), "%s/%s", root, pattern + 2);
        else
            snprintf(resolved, sizeof(resolved), "%s/%s", root, pattern);
        resolved[sizeof(resolved) - 1] = '\0';
        use_pattern = resolved;
    }

    if (!(flags & GLOB_APPEND))
        memset(pglob, 0, sizeof(*pglob));

    strncpy(patbuf, use_pattern, sizeof(patbuf));
    patbuf[sizeof(patbuf) - 1] = '\0';
    espd_unbash_inplace(patbuf);
    {
        size_t n = strlen(patbuf);
        if (n > 0 && patbuf[n - 1] == '/') {
            onlydirs = 1;
            while (n > 1 && patbuf[n - 1] == '/') {
                patbuf[n - 1] = '\0';
                n--;
            }
        }
    }

    slash = strrchr(patbuf, '/');
    if (!slash) {
        strcpy(dirbuf, ".");
        filepat = patbuf;
    } else {
        *slash = '\0';
        filepat = slash + 1;
        if (slash == patbuf)
            strcpy(dirbuf, "/");
        else {
            strncpy(dirbuf, patbuf, sizeof(dirbuf));
            dirbuf[sizeof(dirbuf) - 1] = '\0';
        }
    }

    if (!strchr(filepat, '*') && !strchr(filepat, '?')) {
        struct stat st;
        if (stat(patbuf, &st) == 0) {
            if (!onlydirs || S_ISDIR(st.st_mode))
                return espd_glob_add(pglob, patbuf);
        }
        return GLOB_NOMATCH;
    }

    d = opendir(dirbuf);
    if (!d) {
        return GLOB_NOMATCH;
    }

    while ((de = readdir(d)) != NULL) {
        char full[1200];
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (filepat[0] != '.' && de->d_name[0] == '.')
            continue;
        if (!espd_glob_match(filepat, de->d_name))
            continue;
        if (!strcmp(dirbuf, "/"))
            snprintf(full, sizeof(full), "/%s", de->d_name);
        else if (!strcmp(dirbuf, "."))
            snprintf(full, sizeof(full), "%s", de->d_name);
        else
            snprintf(full, sizeof(full), "%s/%s", dirbuf, de->d_name);
        full[sizeof(full) - 1] = '\0';
        if (onlydirs) {
            struct stat st;
            if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode))
                continue;
        }
        rc = espd_glob_add(pglob, full);
        if (rc) {
            closedir(d);
            return rc;
        }
    }
    closedir(d);
    return pglob->gl_pathc ? 0 : GLOB_NOMATCH;
}

void globfree(glob_t *pglob)
{
    size_t i;
    if (!pglob || !pglob->gl_pathv)
        return;
    for (i = 0; i < pglob->gl_pathc; i++)
        free(pglob->gl_pathv[i]);
    free(pglob->gl_pathv);
    pglob->gl_pathv = NULL;
    pglob->gl_pathc = 0;
}

int lstat(const char *path, struct stat *buf)
{
    return stat(path, buf);
}

uid_t geteuid(void)
{
    return 0;
}
