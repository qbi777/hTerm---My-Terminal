#define _GNU_SOURCE
#include "history.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------- helpers ----------------------------------------------------- */

static char *dupsan(const char *s)            /* copy, TAB/CR/LF -> space   */
{
    char *d = strdup(s ? s : "");
    if (!d) return NULL;
    for (char *p = d; *p; p++)
        if (*p == '\t' || *p == '\n' || *p == '\r') *p = ' ';
    return d;
}

static void entry_free(HistEntry *e)
{
    free(e->user); free(e->cwd); free(e->cmd);
    memset(e, 0, sizeof *e);
}

static size_t entry_len(const HistEntry *e)   /* bytes of its log line      */
{
    char tmp[64];
    int n = snprintf(tmp, sizeof tmp, "%ld\t\t\t%d\t\n", (long)e->ts, e->status);
    return (size_t)n + strlen(e->user) + strlen(e->cwd) + strlen(e->cmd);
}

/* Append, dropping the oldest entry when full. Takes ownership of *e. */
static int push(History *h, HistEntry *e)
{
    if (h->max_entries && h->count >= h->max_entries) {
        entry_free(&h->e[0]);
        memmove(&h->e[0], &h->e[1], (h->count - 1) * sizeof *h->e);
        h->count--;
    }
    if (h->count == h->cap) {
        size_t nc = h->cap ? h->cap * 2 : 64;
        HistEntry *n = realloc(h->e, nc * sizeof *n);
        if (!n) return -1;
        h->e = n; h->cap = nc;
    }
    h->e[h->count++] = *e;
    return 0;
}

static char *next_field(char **p)             /* split on TAB in place      */
{
    char *s = *p, *t = strchr(s, '\t');
    if (t) { *t = '\0'; *p = t + 1; } else *p = s + strlen(s);
    return s;
}

static void ensure_dir(const char *path)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) { *slash = '\0'; mkdir(dir, 0700); }
}

/* ---------- public API -------------------------------------------------- */

int hist_open(History *h, const char *path, size_t max_entries, size_t max_bytes)
{
    memset(h, 0, sizeof *h);
    h->max_entries = max_entries;
    h->max_bytes   = max_bytes;
    snprintf(h->path, sizeof h->path, "%s", path);
    ensure_dir(path);

    FILE *f = fopen(path, "r");
    if (f) {
        char *line = NULL; size_t cap = 0; ssize_t n;
        while ((n = getline(&line, &cap, f)) > 0) {
            h->file_lines++;
            if (line[n - 1] == '\n') line[n - 1] = '\0';
            char *p = line;
            char *ts = next_field(&p), *user = next_field(&p),
                 *cwd = next_field(&p), *st = next_field(&p);
            if (!*p) continue;                         /* malformed / no cmd */
            HistEntry e = { (time_t)atol(ts), strdup(user), strdup(cwd),
                            atoi(st), strdup(p) };
            if (!e.user || !e.cwd || !e.cmd) { entry_free(&e); continue; }
            push(h, &e);
        }
        free(line);
        fclose(f);
        struct stat sb;
        if (stat(path, &sb) == 0) h->file_bytes = (size_t)sb.st_size;
    }
    h->cursor = h->count;
    /* file may hold stale lines from an earlier, bigger limit: trim now */
    if (h->file_lines > h->count || h->file_bytes > h->max_bytes)
        hist_compact(h);
    return 0;
}

void hist_close(History *h)
{
    for (size_t i = 0; i < h->count; i++) entry_free(&h->e[i]);
    free(h->e); free(h->draft);
    memset(h, 0, sizeof *h);
}

int hist_compact(History *h)
{
    /* drop oldest until the whole log fits in 75% of max_bytes */
    size_t total = 0;
    for (size_t i = 0; i < h->count; i++) total += entry_len(&h->e[i]);
    size_t drop = 0, limit = h->max_bytes - h->max_bytes / 4;
    while (drop < h->count && total > limit) total -= entry_len(&h->e[drop++]);
    if (drop) {
        for (size_t i = 0; i < drop; i++) entry_free(&h->e[i]);
        memmove(h->e, h->e + drop, (h->count - drop) * sizeof *h->e);
        h->count -= drop;
    }

    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", h->path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f) { if (fd >= 0) close(fd); return -1; }
    size_t bytes = 0;
    for (size_t i = 0; i < h->count; i++) {
        const HistEntry *e = &h->e[i];
        int n = fprintf(f, "%ld\t%s\t%s\t%d\t%s\n", (long)e->ts, e->user, e->cwd,
                        e->status, e->cmd);
        if (n > 0) bytes += (size_t)n;
    }
    if (fclose(f) != 0 || rename(tmp, h->path) != 0) { unlink(tmp); return -1; }
    h->file_bytes = bytes;
    h->file_lines = h->count;
    h->cursor     = h->count;
    return 0;
}

int hist_add(History *h, const char *user, const char *cwd, int status, const char *cmd)
{
    if (!cmd) return 0;
    if (*cmd == ' ') return 0;                       /* private command      */
    while (*cmd == '\t') cmd++;
    char *c = dupsan(cmd);
    if (!c) return -1;
    size_t len = strlen(c);
    while (len && c[len - 1] == ' ') c[--len] = '\0'; /* rtrim               */
    if (!len) { free(c); return 0; }
    if (h->count && strcmp(h->e[h->count - 1].cmd, c) == 0) {
        free(c); hist_reset_nav(h); return 0;        /* consecutive duplicate*/
    }

    HistEntry e = { time(NULL), dupsan(user), dupsan(cwd), status, c };
    if (!e.user || !e.cwd) { entry_free(&e); return -1; }

    int fd = open(h->path, O_WRONLY | O_CREAT | O_APPEND, 0600);   /* 0600: may   */
    if (fd >= 0) {                                                  /* hold secrets*/
        FILE *f = fdopen(fd, "a");
        if (f) {
            int n = fprintf(f, "%ld\t%s\t%s\t%d\t%s\n", (long)e.ts, e.user, e.cwd,
                            e.status, e.cmd);
            fclose(f);
            if (n > 0) { h->file_bytes += (size_t)n; h->file_lines++; }
        } else close(fd);
    }
    if (push(h, &e) != 0) { entry_free(&e); return -1; }
    h->cursor = h->count;

    if (h->file_bytes > h->max_bytes || h->file_lines > h->max_entries * 2)
        hist_compact(h);                                  /* rotate          */
    return 0;
}

int hist_clear(History *h)
{
    for (size_t i = 0; i < h->count; i++) entry_free(&h->e[i]);
    h->count = h->cursor = h->file_bytes = h->file_lines = 0;
    return truncate(h->path, 0) == 0 || errno == ENOENT ? 0 : -1;
}

const char *hist_prev(History *h, const char *current)
{
    if (!h->count) return NULL;
    if (h->cursor == h->count) {                      /* leaving the draft   */
        free(h->draft);
        h->draft = strdup(current ? current : "");
    }
    if (h->cursor > 0) h->cursor--;
    return h->e[h->cursor].cmd;
}

const char *hist_next(History *h)
{
    if (h->cursor >= h->count) return NULL;
    h->cursor++;
    if (h->cursor == h->count) return h->draft ? h->draft : "";
    return h->e[h->cursor].cmd;
}

void hist_reset_nav(History *h) { h->cursor = h->count; }
