#ifndef HTERM_HISTORY_H
#define HTERM_HISTORY_H
#include <stddef.h>
#include <time.h>
#include <limits.h>

/* One logged command. Stored on disk as one TAB-separated line:
 *   <unix-ts> \t <user> \t <cwd> \t <exit-status> \t <command>             */
typedef struct {
    time_t ts;
    char  *user;
    char  *cwd;
    int    status;
    char  *cmd;
} HistEntry;

typedef struct {
    HistEntry *e;            /* oldest .. newest                            */
    size_t     count, cap;
    size_t     max_entries;  /* in-memory + on-disk entry cap               */
    size_t     max_bytes;    /* on-disk size cap                            */
    size_t     file_bytes;   /* current size of the log file                */
    size_t     file_lines;   /* lines currently in the log file             */
    size_t     cursor;       /* up/down navigation position (count = none)  */
    char      *draft;        /* what the user had typed before pressing Up  */
    char       path[PATH_MAX];
} History;

/* Load (or create) the log. Returns 0 on success, -1 on error. */
int  hist_open(History *h, const char *path, size_t max_entries, size_t max_bytes);
void hist_close(History *h);

/* Record a finished command. Blank commands, commands starting with a space
 * and consecutive duplicates are ignored. Rotates the file when it is too big. */
int  hist_add(History *h, const char *user, const char *cwd, int status, const char *cmd);

/* Rewrite the log so it fits in max_entries / ~75% of max_bytes (drops oldest). */
int  hist_compact(History *h);

/* Wipe memory and file (the `history -c` builtin). */
int  hist_clear(History *h);

/* Up/Down navigation. `current` is the text presently in the input line.
 * The returned pointer is valid until the next history call.                 */
const char *hist_prev(History *h, const char *current);
const char *hist_next(History *h);
void        hist_reset_nav(History *h);

#endif
