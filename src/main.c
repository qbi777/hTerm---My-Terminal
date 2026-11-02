/* hterm - a GTK3 terminal-style developer workstation (iTerm-inspired).
 *   left : split-able terminal panes (line mode + real PTY for vi/nano/less/top/ssh ...)
 *   right: system monitor, file explorer, profile flame, quick commands, git info
 * Build: make   Run: ./hterm                                              */
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <limits.h>
#include <math.h>
#include <pwd.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "config.h"
#include "history.h"
#include "sysinfo.h"
#include "vt.h"

#define SW 64                 /* pixel-scene canvas */
#define SH 40
#define SCENE_FRAMES 170      /* ~14 s per scene at 12 fps */
#define PADX 8
#define PADY 6

#ifdef __APPLE__
#define CMD_MASK (GDK_META_MASK | GDK_SUPER_MASK | GDK_MOD2_MASK)    /* ⌘ */
#else
#define CMD_MASK (GDK_META_MASK | GDK_SUPER_MASK)
#endif

typedef struct {
    gboolean is_repo, detached, has_upstream;
    char name[128], branch[128], remote[64], url[512], web[512], last[256];
    int mod, staged, untracked, ahead, behind;
} GitInfo;

typedef struct Term Term;

typedef struct {
    GtkWidget *win, *status, *clock_lbl, *pane_area;
    Term *cur;                       /* the focused pane: explorer/git/status follow it */
    GList *terms;
    History hist;
    SysInfo *si;
    char user[64], host[128], home[PATH_MAX];
    GitInfo git;
    char *par_path;
    PangoFontDescription *font, *font_bold;
    double cell_w, cell_h;

    /* dashboard */
    GtkWidget *cpu_lbl, *cpu_graph, *core_lbl[4], *core_bar[4];
    GtkWidget *mem_lbl, *mem_bar, *mem_txt, *swap_lbl, *swap_bar, *swap_txt, *task_lbl;
    GtkListStore *store;
    double cpu_hist[GRAPH_N]; int cpu_pos;

    /* explorer */
    GtkWidget *ex_path, *cur_name, *cur_cnt, *par_lbl, *ex_help, *cur_tv, *par_tv;
    GtkTreeStore *cur_store; GtkListStore *par_store;

    /* quick commands + git info */
    GtkWidget *cmd_help, *gi_repo, *gi_branch, *gi_remote, *gi_url, *gi_status, *gi_last, *gi_open;

    /* animated pixel scenes */
    guint32 px[SW * SH];
    guint8 heat[SW * SH], custom[SW * SH];
    gint bolt_x[SH];
    int scene, nscenes, frame, trans, bolt_timer, bolt_age;
    gboolean bolt_on, has_custom;
    GtkWidget *scene_area, *scene_lbl;
} App;

/* One terminal pane. Every pane has its own shell state, scrollback and PTY. */
struct Term {
    App *app;
    GtkWidget *root, *stack, *tv, *vtw;
    GtkTextBuffer *buf;
    GtkTextMark *input_mark, *end_mark;
    char cwd[PATH_MAX], prev_cwd[PATH_MAX];
    gboolean running, out_done, exited, last_nl, closed;
    GPid child; int exit_status;
    GIOChannel *chan; char *pending;
    Vt *vt; guint pty_watch; int close_ticks;
    /* built-in editor page */
    GtkWidget *ed_tv, *ed_title, *ed_info;
    GtkTextBuffer *ed_buf;
    char *ed_path;
    gboolean editing, ed_dirty, ed_loading, ed_new;
    int ed_mode;                        /* 0 plain, 1 C-like, 2 hash-comment */
    guint ed_hl_id;
};

static void refresh_git(App *a);
static void set_current(App *a, Term *t);
static void edit_open(Term *t, const char *arg, const char *orig);
static void refresh_explorer(App *a);
static void navigate_to(App *a, const char *path);
static gchar *git_run(const char *cwd, ...) G_GNUC_NULL_TERMINATED;

/* ===================================================================== */
/*  Terminal buffer helpers (line mode)                                   */
/* ===================================================================== */

static void term_append(Term *t, const char *text, const char *tag)
{
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(t->buf, &end);
    if (tag) gtk_text_buffer_insert_with_tags_by_name(t->buf, &end, text, -1, tag, NULL);
    else     gtk_text_buffer_insert(t->buf, &end, text, -1);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(t->tv), t->end_mark, 0, FALSE, 0, 0);
}

static void term_appendf(Term *t, const char *tag, const char *fmt, ...) G_GNUC_PRINTF(3, 4);
static void term_appendf(Term *t, const char *tag, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    char *s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    term_append(t, s, tag);
    g_free(s);
}

static char *get_input(Term *t)
{
    GtkTextIter s, e;
    gtk_text_buffer_get_iter_at_mark(t->buf, &s, t->input_mark);
    gtk_text_buffer_get_end_iter(t->buf, &e);
    return gtk_text_buffer_get_text(t->buf, &s, &e, FALSE);
}

static void set_input(Term *t, const char *text)
{
    GtkTextIter s, e;
    gtk_text_buffer_get_iter_at_mark(t->buf, &s, t->input_mark);
    gtk_text_buffer_get_end_iter(t->buf, &e);
    gtk_text_buffer_delete(t->buf, &s, &e);
    gtk_text_buffer_get_end_iter(t->buf, &e);
    gtk_text_buffer_insert(t->buf, &e, text, -1);
    gtk_text_buffer_get_end_iter(t->buf, &e);
    gtk_text_buffer_place_cursor(t->buf, &e);
}

static void display_cwd(Term *t, char *out, size_t n)
{
    const char *home = t->app->home;
    size_t hl = strlen(home);
    if (hl && !strncmp(t->cwd, home, hl) && (t->cwd[hl] == '/' || !t->cwd[hl]))
        g_snprintf(out, n, "~%s", t->cwd + hl);
    else
        g_strlcpy(out, t->cwd, n);
}

static void focus_term(Term *t)
{
    gtk_widget_grab_focus(t->vt ? t->vtw : t->tv);
}

static void update_status(App *a)
{
    Term *t = a->cur;
    if (!t) return;
    char dc[PATH_MAX]; display_cwd(t, dc, sizeof dc);
    time_t now = time(NULL); char tm[16];
    strftime(tm, sizeof tm, "%H:%M:%S", localtime(&now));
    char *ecwd = g_markup_escape_text(dc, -1), *ebr = g_markup_escape_text(a->git.branch, -1);
    char *m = g_strdup_printf(
        "<span foreground='#00ff9c'>%s@%s</span>   %s   %s%s   <span foreground='%s'>● %s</span>"
        "   panes: %u   history: %zu   <span foreground='#5b7fa0'>⌘D split right · ⌘⇧D split below · ⌘W close pane</span>"
        "   %s",
        a->user, a->host, ecwd,
        a->git.is_repo ? "⎇ " : "", ebr,
        t->running ? "#ffb454" : "#00ff9c", t->running ? "running" : "ready",
        g_list_length(a->terms), a->hist.count, tm);
    gtk_label_set_markup(GTK_LABEL(a->status), m);
    gtk_label_set_text(GTK_LABEL(a->clock_lbl), tm);
    g_free(m); g_free(ecwd); g_free(ebr);
}

static void pane_branch(const char *cwd, char *out, size_t n)
{
    out[0] = '\0';
    gchar *o = git_run(cwd, "branch", "--show-current", NULL);
    if (o) { g_strstrip(o); g_strlcpy(out, o, n); g_free(o); }
}

static void show_prompt(Term *t)
{
    App *a = t->app;
    gint lines = gtk_text_buffer_get_line_count(t->buf);                 /* cap scrollback */
    if (lines > TERM_MAX_LINES) {
        GtkTextIter s, e;
        gtk_text_buffer_get_start_iter(t->buf, &s);
        gtk_text_buffer_get_iter_at_line(t->buf, &e, lines - TERM_MAX_LINES);
        gtk_text_buffer_delete(t->buf, &s, &e);
    }
    if (t == a->cur) { refresh_git(a); refresh_explorer(a); }
    char dc[PATH_MAX], br[128];
    display_cwd(t, dc, sizeof dc);
    pane_branch(t->cwd, br, sizeof br);
    term_appendf(t, "prompt", "%s@%s", a->user, a->host);
    term_appendf(t, "prompt_sep", ":");
    term_appendf(t, "prompt_dir", "%s", dc);
    if (*br) term_appendf(t, "prompt_git", " (%s)", br);
    term_append(t, "$ ", "prompt");

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(t->buf, &end);
    gtk_text_buffer_move_mark(t->buf, t->input_mark, &end);
    gtk_text_buffer_place_cursor(t->buf, &end);
    hist_reset_nav(&a->hist);
    t->last_nl = TRUE;
    update_status(a);
}

/* ===================================================================== */
/*  Command execution (line mode)                                         */
/* ===================================================================== */

static void record(Term *t, const char *cmd, int status)
{
    hist_add(&t->app->hist, t->app->user, t->cwd, status, cmd);
}

static void finish_command(Term *t)
{
    int code = WIFEXITED(t->exit_status)   ? WEXITSTATUS(t->exit_status)
             : WIFSIGNALED(t->exit_status) ? 128 + WTERMSIG(t->exit_status) : 1;
    if (!t->last_nl) term_append(t, "\n", NULL);
    if (code) term_appendf(t, "err", "[exit %d]\n", code);
    record(t, t->pending ? t->pending : "", code);
    g_free(t->pending); t->pending = NULL;
    t->running = FALSE; t->child = 0;
    show_prompt(t);
}

static void maybe_finish(Term *t)
{
    if (t->running && t->out_done && t->exited) finish_command(t);
}

static gboolean on_out(GIOChannel *ch, GIOCondition cond, gpointer d)
{
    (void)cond;
    Term *t = d;
    if (t->closed) {
        g_io_channel_shutdown(ch, FALSE, NULL); g_io_channel_unref(ch);
        t->chan = NULL;
        return G_SOURCE_REMOVE;
    }
    gchar buf[4096]; gsize n = 0; GIOStatus s;
    do {
        s = g_io_channel_read_chars(ch, buf, sizeof buf, &n, NULL);
        if (n > 0) {
            gchar *v = g_utf8_make_valid(buf, (gssize)n);
            if (*v) { term_append(t, v, "out"); t->last_nl = v[strlen(v) - 1] == '\n'; }
            g_free(v);
        }
    } while (s == G_IO_STATUS_NORMAL);
    if (s == G_IO_STATUS_AGAIN) return G_SOURCE_CONTINUE;
    g_io_channel_shutdown(ch, FALSE, NULL);
    g_io_channel_unref(ch);
    t->chan = NULL; t->out_done = TRUE;
    maybe_finish(t);
    return G_SOURCE_REMOVE;
}

static void on_child(GPid pid, gint status, gpointer d)
{
    Term *t = d;
    g_spawn_close_pid(pid);
    if (t->closed) return;
    t->exit_status = status; t->exited = TRUE; t->child = 0;
    maybe_finish(t);
}

static void child_setup(gpointer d) { (void)d; setpgid(0, 0); }   /* own group → Ctrl+C kills tree */

static void spawn_command(Term *t, const char *line)
{
    char *script = g_strdup_printf("{ %s\n} 2>&1", line);        /* merge stderr into stdout */
    gchar *argv[] = { "/bin/sh", "-c", script, NULL };
    GError *err = NULL; gint in_fd = -1, out_fd = -1; GPid pid;
    gboolean ok = g_spawn_async_with_pipes(t->cwd, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
                                           child_setup, NULL, &pid, &in_fd, &out_fd, NULL, &err);
    g_free(script);
    if (!ok) {
        term_appendf(t, "err", "hterm: cannot run command: %s\n", err->message);
        g_error_free(err);
        record(t, line, 127);
        show_prompt(t);
        return;
    }
    close(in_fd);                      /* line mode has no stdin; interactive tools use the PTY path */
    t->running = TRUE; t->out_done = t->exited = FALSE; t->child = pid;
    t->pending = g_strdup(line);
    t->chan = g_io_channel_unix_new(out_fd);
    g_io_channel_set_encoding(t->chan, NULL, NULL);
    g_io_channel_set_close_on_unref(t->chan, TRUE);
    g_io_channel_set_flags(t->chan, G_IO_FLAG_NONBLOCK, NULL);
    g_io_add_watch(t->chan, G_IO_IN | G_IO_HUP | G_IO_ERR, on_out, t);
    g_child_watch_add(pid, on_child, t);
    update_status(t->app);
}

/* ===================================================================== */
/*  Real terminal mode (PTY + emulator) for vi, nano, less, top, ssh ...   */
/* ===================================================================== */

static const char *const TTY_PROGS[] = {
    "vi", "vim", "nvim", "view", "vimdiff", "nano", "pico", "emacs", "less", "more", "most", "man",
    "top", "htop", "btop", "atop", "ssh", "sftp", "ftp", "telnet", "tmux", "screen", "watch",
    "passwd", "su", "sudo", "visudo", "crontab", NULL };
static const char *const REPL_PROGS[] = {       /* interactive only when started without arguments */
    "python", "python3", "node", "irb", "ruby", "lua", "ipython", "sqlite3", "psql", "mysql",
    "redis-cli", "bash", "zsh", "sh", "fish", "gdb", "lldb", "bc", NULL };
static const char *const WRAPPERS[] = { "env", "time", "nohup", "command", "exec", NULL };

static gboolean in_list(const char *const *l, const char *s)
{
    for (; *l; l++) if (!strcmp(*l, s)) return TRUE;
    return FALSE;
}

static gboolean needs_tty(const char *line)
{
    gchar **tok = g_strsplit_set(line, " \t", -1);
    gboolean res = FALSE;
    int i = 0, n = 0;
    while (tok[n]) n++;
    while (i < n && (!*tok[i] || strchr(tok[i], '=') || in_list(WRAPPERS, tok[i]))) i++;  /* FOO=1 env time vi */
    if (i < n) {
        char *base = g_path_get_basename(tok[i]);
        int rest = 0;
        for (int j = i + 1; j < n; j++) if (*tok[j]) rest++;
        if (strpbrk(line, "|;&<>")) res = FALSE;                       /* pipelines/redirects stay in line mode */
        else if (in_list(TTY_PROGS, base)) res = TRUE;
        else if (in_list(REPL_PROGS, base)) res = (rest == 0);
        else if (!strcmp(base, "git") && i + 1 < n) {                    /* editor-launching git commands */
            const char *sub = tok[i + 1];
            if (!strcmp(sub, "commit") || !strcmp(sub, "merge") || !strcmp(sub, "tag")) {
                gboolean has_msg = FALSE;
                for (int j = i + 2; j < n; j++)
                    if ((tok[j][0] == '-' && tok[j][1] != '-' && strchr(tok[j], 'm')) ||      /* -m, -am */
                        !strncmp(tok[j], "--message", 9) ||
                        !strcmp(tok[j], "-F") || !strcmp(tok[j], "--no-edit") || !strcmp(tok[j], "-C") ||
                        !strcmp(tok[j], "--amend=no") || !strcmp(tok[j], "-h"))
                        has_msg = TRUE;
                res = !has_msg && strcmp(sub, "tag") != 0;
            } else if (!strcmp(sub, "rebase")) {
                for (int j = i + 2; j < n; j++) if (!strcmp(tok[j], "-i") || !strcmp(tok[j], "--interactive")) res = TRUE;
            } else if (!strcmp(sub, "add")) {
                for (int j = i + 2; j < n; j++) if (!strcmp(tok[j], "-p") || !strcmp(tok[j], "-i")) res = TRUE;
            }
        }
        g_free(base);
    }
    g_strfreev(tok);
    return res;
}

static void term_vt_size(Term *t, int *cols, int *rows)
{
    App *a = t->app;
    int w = gtk_widget_get_allocated_width(t->stack), h = gtk_widget_get_allocated_height(t->stack);
    if (w < 120 || h < 80) { *cols = 80; *rows = 24; return; }
    *cols = (int)((w - 2 * PADX) / a->cell_w);
    *rows = (int)((h - 2 * PADY) / a->cell_h);
    if (*cols < 20) *cols = 20;
    if (*rows < 5) *rows = 5;
}

static void dump_screen(Term *t, Vt *v)             /* keep non-fullscreen output (sudo ls, python REPL...) */
{
    int last = -1;
    char *row = g_malloc((gsize)v->cols * 4 + 8);
    for (int r = v->rows - 1; r >= 0 && last < 0; r--) {
        vt_row_text(v, r, row, (size_t)v->cols * 4 + 8);
        if (*row) last = r;
    }
    for (int r = 0; r <= last; r++) {
        vt_row_text(v, r, row, (size_t)v->cols * 4 + 8);
        term_append(t, row, "out"); term_append(t, "\n", "out");
    }
    t->last_nl = TRUE;
    g_free(row);
}

static void finish_pty(Term *t)
{
    Vt *v = t->vt;
    t->vt = NULL;
    gtk_stack_set_visible_child_name(GTK_STACK(t->stack), "text");
    if (v) {
        if (!v->ever_alt) dump_screen(t, v);
        vt_free(v);
    }
    finish_command(t);
    gtk_widget_grab_focus(t->tv);
}

/* Polled every ~12 ms: robust on macOS where poll() on pty devices is unreliable. */
static gboolean pty_poll(gpointer d)
{
    Term *t = d;
    Vt *v = t->vt;
    if (!v) { t->pty_watch = 0; return G_SOURCE_REMOVE; }
    int st = 0;
    if (t->closed) {                                  /* pane closed while the program ran */
        if (t->close_ticks++ == 0) vt_hangup(v);
        if (t->close_ticks > 25) kill(v->pid, SIGKILL);
        if (waitpid(v->pid, &st, WNOHANG) == v->pid || t->close_ticks > 60) {
            t->vt = NULL; vt_free(v); t->pty_watch = 0;
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }
    int n = vt_read(v);
    if (n > 0) gtk_widget_queue_draw(t->vtw);
    if (waitpid(v->pid, &st, WNOHANG) == v->pid) {
        vt_read(v);                                   /* drain what is left */
        t->exit_status = st;
        t->pty_watch = 0;
        finish_pty(t);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean focus_idle(gpointer d)
{
    Term *t = d;
    if (!t->closed) focus_term(t);
    return G_SOURCE_REMOVE;
}

static void pty_start(Term *t, const char *cmd, const char *orig)
{
    int cols, rows;
    term_vt_size(t, &cols, &rows);
    Vt *v = vt_new(rows, cols);
    const char *sh = g_getenv("SHELL");
    if (!sh || !*sh) sh = "/bin/sh";
    char *argv[] = { (char *)sh, "-c", (char *)cmd, NULL };
    if (!v || vt_spawn(v, t->cwd, argv) != 0) {
        vt_free(v);
        /* could not get a pty: fall back to the built-in editor for "vi file" style commands */
        gchar **tok = g_strsplit_set(cmd, " \t", -1);
        if (tok[0] && tok[1] && *tok[1] && (!strcmp(tok[0], "vi") || !strcmp(tok[0], "vim") || !strcmp(tok[0], "nano") ||
                                               !strcmp(tok[0], "nvim") || !strcmp(tok[0], "pico"))) {
            term_append(t, "hterm: no pseudo-terminal available — opening the built-in editor instead\n", "info");
            edit_open(t, tok[1], orig);
            g_strfreev(tok);
            return;
        }
        g_strfreev(tok);
        term_append(t, "hterm: cannot open a pseudo-terminal\n", "err");
        record(t, orig, 127);
        show_prompt(t);
        return;
    }
    t->vt = v;
    t->running = TRUE; t->pending = g_strdup(orig); t->close_ticks = 0;
    t->pty_watch = g_timeout_add(12, pty_poll, t);
    gtk_widget_show(t->vtw);
    gtk_stack_set_visible_child_name(GTK_STACK(t->stack), "vt");
    gtk_widget_grab_focus(t->vtw);
    g_idle_add(focus_idle, t);                          /* again once the page is mapped */
    update_status(t->app);
}

/* ---- keyboard → escape sequences ---- */

static int encode_key(Vt *v, GdkEventKey *ev, char *o)
{
    guint k = ev->keyval;
    gboolean ctrl  = (ev->state & GDK_CONTROL_MASK) != 0, alt = (ev->state & GDK_MOD1_MASK) != 0,
             shift = (ev->state & GDK_SHIFT_MASK) != 0;
    int mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    const char *s = NULL; char tmp[24];
    switch (k) {
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: s = "\r"; break;
    case GDK_KEY_BackSpace: s = ctrl ? "\x08" : "\x7f"; break;
    case GDK_KEY_Tab: s = "\t"; break;
    case GDK_KEY_ISO_Left_Tab: s = "\033[Z"; break;
    case GDK_KEY_Escape: s = "\033"; break;
    case GDK_KEY_Up: case GDK_KEY_Down: case GDK_KEY_Right: case GDK_KEY_Left: {
        char c = k == GDK_KEY_Up ? 'A' : k == GDK_KEY_Down ? 'B' : k == GDK_KEY_Right ? 'C' : 'D';
        if (mod > 1) g_snprintf(tmp, sizeof tmp, "\033[1;%d%c", mod, c);
        else if (v->app_cursor) g_snprintf(tmp, sizeof tmp, "\033O%c", c);
        else g_snprintf(tmp, sizeof tmp, "\033[%c", c);
        s = tmp; break; }
    case GDK_KEY_Home: case GDK_KEY_KP_Home: s = v->app_cursor ? "\033OH" : "\033[H"; break;
    case GDK_KEY_End: case GDK_KEY_KP_End: s = v->app_cursor ? "\033OF" : "\033[F"; break;
    case GDK_KEY_Page_Up: s = "\033[5~"; break;
    case GDK_KEY_Page_Down: s = "\033[6~"; break;
    case GDK_KEY_Delete: s = "\033[3~"; break;
    case GDK_KEY_Insert: s = "\033[2~"; break;
    case GDK_KEY_F1: s = "\033OP"; break;   case GDK_KEY_F2: s = "\033OQ"; break;
    case GDK_KEY_F3: s = "\033OR"; break;   case GDK_KEY_F4: s = "\033OS"; break;
    case GDK_KEY_F5: s = "\033[15~"; break; case GDK_KEY_F6: s = "\033[17~"; break;
    case GDK_KEY_F7: s = "\033[18~"; break; case GDK_KEY_F8: s = "\033[19~"; break;
    case GDK_KEY_F9: s = "\033[20~"; break; case GDK_KEY_F10: s = "\033[21~"; break;
    case GDK_KEY_F11: s = "\033[23~"; break; case GDK_KEY_F12: s = "\033[24~"; break;
    default: {
        guint32 u = gdk_keyval_to_unicode(k);
        if (!u) return 0;
        if (ctrl) {
            int c;
            if (u >= 'a' && u <= 'z') c = (int)(u - 'a' + 1);
            else if (u >= 'A' && u <= 'Z') c = (int)(u - 'A' + 1);
            else if (u == '@' || u == ' ' || u == '2') c = 0;
            else if (u == '[') c = 27;
            else if (u == '\\') c = 28;
            else if (u == ']') c = 29;
            else if (u == '^' || u == '6') c = 30;
            else if (u == '_' || u == '/') c = 31;
            else return 0;
            o[0] = (char)c;
            return 1;
        }
        int n = g_unichar_to_utf8(u, o);
#ifndef __APPLE__
        if (alt) { memmove(o + 1, o, (size_t)n); o[0] = 27; n++; }       /* Meta = ESC prefix */
#endif
        return n; }
    }
    size_t n = strlen(s);
    memcpy(o, s, n);
    return (int)n;
}

static void on_paste_text(GtkClipboard *cb, const gchar *text, gpointer d)
{
    (void)cb;
    Term *t = d;
    if (!text || !t->vt || t->closed) return;
    if (t->vt->bracketed_paste) vt_write(t->vt, "\033[200~", 6);
    vt_write(t->vt, text, strlen(text));
    if (t->vt->bracketed_paste) vt_write(t->vt, "\033[201~", 6);
}

static gboolean on_vt_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w;
    Term *t = d;
    if (!t->vt) return TRUE;
    guint k = ev->keyval;
    gboolean cmd = (ev->state & CMD_MASK) != 0, ctrl = (ev->state & GDK_CONTROL_MASK) != 0,
             shift = (ev->state & GDK_SHIFT_MASK) != 0;
    if ((cmd || (ctrl && shift)) && (k == GDK_KEY_v || k == GDK_KEY_V)) {   /* paste */
        gtk_clipboard_request_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), on_paste_text, t);
        return TRUE;
    }
    if (cmd) return TRUE;                         /* ⌘-shortcuts are handled by the window */
    char out[32];
    int n = encode_key(t->vt, ev, out);
    if (n > 0) vt_write(t->vt, out, (size_t)n);
    return TRUE;
}

static void set_rgb(cairo_t *cr, uint32_t c, uint32_t def, double scale)
{
    uint32_t rgb = c ? (c & 0xFFFFFF) : def;
    cairo_set_source_rgb(cr, ((rgb >> 16) & 0xFF) / 255.0 * scale, ((rgb >> 8) & 0xFF) / 255.0 * scale,
                         (rgb & 0xFF) / 255.0 * scale);
}

static gboolean draw_vt(GtkWidget *w, cairo_t *cr, gpointer d)
{
    Term *t = d;
    App *a = t->app;
    Vt *v = t->vt;
    const uint32_t DEF_FG = 0xd8f3ff, DEF_BG = 0x050a18;
    cairo_set_source_rgb(cr, 5 / 255.0, 10 / 255.0, 24 / 255.0);
    cairo_paint(cr);
    if (!v) return FALSE;

    PangoLayout *lay = pango_cairo_create_layout(cr);
    double cw = a->cell_w, ch = a->cell_h;
    for (int r = 0; r < v->rows; r++) {
        double y = PADY + r * ch;
        int c = 0;
        while (c < v->cols) {
            VtCell *cell = &v->cells[(size_t)r * v->cols + c];
            uint32_t fg = cell->fg, bg = cell->bg; uint8_t at = cell->attr;
            uint32_t efg = fg ? (fg & 0xFFFFFF) : DEF_FG, ebg = bg ? (bg & 0xFFFFFF) : DEF_BG;
            if (at & VT_REV) { uint32_t tmp = efg; efg = ebg; ebg = tmp; }
            int end = c + 1;
            while (end < v->cols) {                                     /* run of identical style */
                VtCell *n = &v->cells[(size_t)r * v->cols + end];
                if (n->fg != fg || n->bg != bg || n->attr != at) break;
                end++;
            }
            if (ebg != DEF_BG) {
                set_rgb(cr, (1u << 24) | ebg, DEF_BG, 1.0);
                cairo_rectangle(cr, PADX + c * cw, y, (end - c) * cw + 0.5, ch);
                cairo_fill(cr);
            }
            set_rgb(cr, (1u << 24) | efg, DEF_FG, (at & VT_DIM) ? 0.6 : 1.0);
            pango_layout_set_font_description(lay, (at & VT_BOLD) ? a->font_bold : a->font);
            GString *run = g_string_new(NULL);
            int run_start = c;
            for (int x = c; x <= end; x++) {
                uint32_t ch_cp = x < end ? v->cells[(size_t)r * v->cols + x].ch : 0;
                gboolean ascii = x < end && ch_cp >= 32 && ch_cp < 127;
                if (ascii) { g_string_append_c(run, (char)ch_cp); continue; }
                if (run->len) {                                          /* flush ASCII span */
                    gboolean blank_run = TRUE;
                    for (gsize q = 0; q < run->len; q++) if (run->str[q] != ' ') { blank_run = FALSE; break; }
                    if (!blank_run) {
                        pango_layout_set_text(lay, run->str, (int)run->len);
                        cairo_move_to(cr, PADX + run_start * cw, y);
                        pango_cairo_show_layout(cr, lay);
                    }
                    g_string_set_size(run, 0);
                }
                if (x < end && ch_cp > 127) {                            /* non-ASCII: one glyph per cell */
                    char u8[8]; int n8 = g_unichar_to_utf8(ch_cp, u8);
                    pango_layout_set_text(lay, u8, n8);
                    cairo_move_to(cr, PADX + x * cw, y);
                    pango_cairo_show_layout(cr, lay);
                }
                run_start = x + 1;
            }
            g_string_free(run, TRUE);
            if (at & VT_UL) {
                cairo_rectangle(cr, PADX + c * cw, y + ch - 1.5, (end - c) * cw, 1);
                cairo_fill(cr);
            }
            c = end;
        }
    }
    g_object_unref(lay);

    if (v->cursor_visible && v->cy < v->rows && v->cx < v->cols) {        /* cursor */
        double x = PADX + v->cx * cw, y = PADY + v->cy * ch;
        if (gtk_widget_has_focus(w)) {
            cairo_set_source_rgba(cr, 0, 1, 0.61, 0.55);
            cairo_rectangle(cr, x, y, cw, ch);
            cairo_fill(cr);
        } else {
            cairo_set_source_rgba(cr, 0, 1, 0.61, 0.8);
            cairo_set_line_width(cr, 1);
            cairo_rectangle(cr, x + .5, y + .5, cw - 1, ch - 1);
            cairo_stroke(cr);
        }
    }
    return FALSE;
}

static void on_vt_size(GtkWidget *w, GdkRectangle *r, gpointer d)
{
    (void)w;
    Term *t = d;
    if (!t->vt) return;
    int cols = (int)((r->width - 2 * PADX) / t->app->cell_w), rows = (int)((r->height - 2 * PADY) / t->app->cell_h);
    if (cols >= 2 && rows >= 2) vt_resize(t->vt, rows, cols);           /* also sends SIGWINCH */
}

static gboolean on_vt_focus(GtkWidget *w, GdkEventFocus *ev, gpointer d)
{
    Term *t = d;
    gtk_widget_queue_draw(w);                          /* hollow cursor when unfocused, solid when focused */
    if (ev->in && !t->closed) set_current(t->app, t);
    return FALSE;
}

static gboolean on_vt_click(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
    (void)ev; (void)d;
    gtk_widget_grab_focus(w);
    return TRUE;
}

/* ===================================================================== */
/*  Built-in editor:  edit <file>   (Ctrl+S save · Esc close · Ctrl+Q discard) */
/* ===================================================================== */

static const char *const ED_KW[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return", "goto",
    "sizeof", "typedef", "struct", "union", "enum", "static", "extern", "const", "volatile", "inline",
    "register", "class", "public", "private", "protected", "new", "delete", "import", "from", "def", "function",
    "var", "let", "in", "is", "not", "and", "or", "True", "False", "None", "NULL", "true", "false", "fn", "pub", NULL };
static const char *const ED_TY[] = {
    "int", "char", "short", "long", "float", "double", "void", "unsigned", "signed", "size_t", "ssize_t", "bool",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t", "FILE", "string",
    "String", "auto", NULL };

static void ed_tags(GtkTextBuffer *b)
{
    gtk_text_buffer_create_tag(b, "kw",  "foreground", "#c792ea", "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(b, "ty",  "foreground", "#3aa0ff", NULL);
    gtk_text_buffer_create_tag(b, "str", "foreground", "#ffb454", NULL);
    gtk_text_buffer_create_tag(b, "com", "foreground", "#5b7fa0", "style", PANGO_STYLE_ITALIC, NULL);
    gtk_text_buffer_create_tag(b, "num", "foreground", "#00e5a0", NULL);
    gtk_text_buffer_create_tag(b, "pre", "foreground", "#ff8099", NULL);
}

static void ed_apply(GtkTextBuffer *b, long from, long to, const char *tag)
{
    GtkTextIter s, e;
    gtk_text_buffer_get_iter_at_offset(b, &s, (gint)from);
    gtk_text_buffer_get_iter_at_offset(b, &e, (gint)to);
    gtk_text_buffer_apply_tag_by_name(b, tag, &s, &e);
}

static void ed_highlight(Term *t)
{
    GtkTextBuffer *b = t->ed_buf;
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    gtk_text_buffer_remove_all_tags(b, &s, &e);
    if (t->ed_mode == 0) return;
    char *txt = gtk_text_buffer_get_text(b, &s, &e, FALSE);
    size_t n = strlen(txt), i = 0;
    if (n > 200000) { g_free(txt); return; }
    long ch = 0;
    gboolean bol = TRUE;                                   /* only whitespace so far on this line */
    while (i < n) {
        unsigned char c = (unsigned char)txt[i];
        size_t j = i; const char *tag = NULL;
        if (t->ed_mode == 1 && c == '/' && txt[i + 1] == '/') { while (j < n && txt[j] != '\n') j++; tag = "com"; }
        else if (t->ed_mode == 1 && c == '/' && txt[i + 1] == '*') {
            j = i + 2; while (j < n && !(txt[j] == '*' && txt[j + 1] == '/')) j++;
            j = j < n ? j + 2 : n; tag = "com";
        }
        else if (t->ed_mode == 2 && c == '#') { while (j < n && txt[j] != '\n') j++; tag = "com"; }
        else if (t->ed_mode == 1 && c == '#' && bol) { while (j < n && txt[j] != '\n') j++; tag = "pre"; }
        else if (c == '"' || c == '\'') {
            j = i + 1;
            while (j < n && txt[j] != (char)c && txt[j] != '\n') { if (txt[j] == '\\' && j + 1 < n) j++; j++; }
            if (j < n && txt[j] == (char)c) j++;
            tag = "str";
        }
        else if (g_ascii_isdigit((gchar)c)) { while (j < n && (g_ascii_isalnum(txt[j]) || txt[j] == '.')) j++; tag = "num"; }
        else if (g_ascii_isalpha((gchar)c) || c == '_') {
            while (j < n && (g_ascii_isalnum(txt[j]) || txt[j] == '_')) j++;
            char *w = g_strndup(txt + i, j - i);
            if (in_list(ED_KW, w)) tag = "kw"; else if (in_list(ED_TY, w)) tag = "ty";
            g_free(w);
        }
        if (!tag) {
            if (c == '\n') bol = TRUE; else if (c != ' ' && c != '\t') bol = FALSE;
            if ((c & 0xC0) != 0x80) ch++;
            i++;
            continue;
        }
        if (j == i) j = i + 1;
        long len = (long)g_utf8_strlen(txt + i, (gssize)(j - i));
        ed_apply(b, ch, ch + len, tag);
        ch += len; i = j; bol = FALSE;
    }
    g_free(txt);
}

static gboolean ed_hl_idle(gpointer d)
{
    Term *t = d;
    t->ed_hl_id = 0;
    if (!t->closed && t->editing) ed_highlight(t);
    return G_SOURCE_REMOVE;
}

static void ed_update_info(Term *t, const char *msg)
{
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(t->ed_buf, &it, gtk_text_buffer_get_insert(t->ed_buf));
    char *s = g_strdup_printf("Ln %d, Col %d   %s   Ctrl+S save · Esc close · Ctrl+Q discard",
                              gtk_text_iter_get_line(&it) + 1, gtk_text_iter_get_line_offset(&it) + 1, msg ? msg : "");
    gtk_label_set_text(GTK_LABEL(t->ed_info), s);
    g_free(s);
}

static void ed_update_title(Term *t)
{
    char *s = g_strdup_printf("✎ %s%s%s", t->ed_path ? t->ed_path : "", t->ed_new ? "  (new file)" : "",
                              t->ed_dirty ? "  ● modified" : "");
    gtk_label_set_text(GTK_LABEL(t->ed_title), s);
    g_free(s);
}

static void on_ed_changed(GtkTextBuffer *b, gpointer d)
{
    (void)b;
    Term *t = d;
    if (t->ed_loading || t->closed) return;
    t->ed_dirty = TRUE;
    ed_update_title(t);
    if (!t->ed_hl_id) t->ed_hl_id = g_idle_add(ed_hl_idle, t);        /* re-highlight when idle */
}

static void on_ed_cursor(GObject *o, GParamSpec *p, gpointer d)
{
    (void)o; (void)p;
    Term *t = d;
    if (t->editing && !t->closed) ed_update_info(t, NULL);
}

static void ed_save(Term *t)
{
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(t->ed_buf, &s, &e);
    char *txt = gtk_text_buffer_get_text(t->ed_buf, &s, &e, FALSE);
    GError *err = NULL;
    if (g_file_set_contents(t->ed_path, txt, -1, &err)) {
        t->ed_dirty = FALSE; t->ed_new = FALSE;
        time_t now = time(NULL); char tm[16];
        strftime(tm, sizeof tm, "%H:%M:%S", localtime(&now));
        char *m = g_strdup_printf("✔ saved %s", tm);
        ed_update_title(t); ed_update_info(t, m);
        g_free(m);
    } else {
        char *m = g_strdup_printf("✖ cannot save: %s", err->message);
        ed_update_info(t, m);
        g_free(m); g_error_free(err);
    }
    g_free(txt);
}

static void ed_close(Term *t)
{
    t->editing = FALSE;
    if (t->ed_hl_id) { g_source_remove(t->ed_hl_id); t->ed_hl_id = 0; }
    gtk_stack_set_visible_child_name(GTK_STACK(t->stack), "text");
    g_free(t->ed_path); t->ed_path = NULL;
    t->exit_status = 0; t->last_nl = TRUE;
    finish_command(t);                                  /* logs the command, shows a fresh prompt */
    gtk_widget_grab_focus(t->tv);
}

static gboolean on_ed_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w;
    Term *t = d;
    guint k = ev->keyval;
    gboolean ctl = (ev->state & (GDK_CONTROL_MASK | CMD_MASK)) != 0, ctrl = (ev->state & GDK_CONTROL_MASK) != 0;
    if (ctl && (k == GDK_KEY_s || k == GDK_KEY_S)) { ed_save(t); return TRUE; }
    if (ctrl && (k == GDK_KEY_q || k == GDK_KEY_Q)) { ed_close(t); return TRUE; }
    if (k == GDK_KEY_Escape) {
        if (t->ed_dirty) ed_update_info(t, "⚠ unsaved changes!");
        else ed_close(t);
        return TRUE;
    }
    if (k == GDK_KEY_Tab && !ctl) { gtk_text_buffer_insert_at_cursor(t->ed_buf, "    ", 4); return TRUE; }
    if ((k == GDK_KEY_Return || k == GDK_KEY_KP_Enter) && !ctl) {                      /* auto-indent */
        GtkTextIter it, ls;
        gtk_text_buffer_get_iter_at_mark(t->ed_buf, &it, gtk_text_buffer_get_insert(t->ed_buf));
        ls = it; gtk_text_iter_set_line_offset(&ls, 0);
        char *before = gtk_text_buffer_get_text(t->ed_buf, &ls, &it, FALSE);
        size_t ind = 0;
        while (before[ind] == ' ' || before[ind] == '\t') ind++;
        size_t l = strlen(before);
        while (l > 0 && (before[l - 1] == ' ' || before[l - 1] == '\t')) l--;
        gboolean extra = l > 0 && (before[l - 1] == '{' || (t->ed_mode == 2 && before[l - 1] == ':'));
        GString *ins = g_string_new("\n");
        g_string_append_len(ins, before, (gssize)ind);
        if (extra) g_string_append(ins, "    ");
        gtk_text_buffer_insert_at_cursor(t->ed_buf, ins->str, (gint)ins->len);
        gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(t->ed_tv), gtk_text_buffer_get_insert(t->ed_buf));
        g_string_free(ins, TRUE); g_free(before);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_ed_draw(GtkWidget *w, cairo_t *cr, gpointer d)           /* line-number gutter */
{
    Term *t = d;
    GtkTextView *tv = GTK_TEXT_VIEW(w);
    GdkWindow *win = gtk_text_view_get_window(tv, GTK_TEXT_WINDOW_LEFT);
    if (!win || !gtk_cairo_should_draw_window(cr, win)) return FALSE;
    gtk_cairo_transform_to_window(cr, w, win);
    int ww = gdk_window_get_width(win), wh = gdk_window_get_height(win);
    cairo_set_source_rgb(cr, 0.03, 0.07, 0.12);
    cairo_rectangle(cr, 0, 0, ww, wh);
    cairo_fill(cr);

    GdkRectangle vis;
    gtk_text_view_get_visible_rect(tv, &vis);
    GtkTextIter it;
    gtk_text_view_get_line_at_y(tv, &it, vis.y, NULL);
    GtkTextIter cur;
    gtk_text_buffer_get_iter_at_mark(t->ed_buf, &cur, gtk_text_buffer_get_insert(t->ed_buf));
    int cur_line = gtk_text_iter_get_line(&cur);
    PangoLayout *lay = gtk_widget_create_pango_layout(w, NULL);
    pango_layout_set_font_description(lay, t->app->font);
    for (;;) {
        int y, h, wx, wy;
        int line = gtk_text_iter_get_line(&it);
        gtk_text_view_get_line_yrange(tv, &it, &y, &h);
        gtk_text_view_buffer_to_window_coords(tv, GTK_TEXT_WINDOW_LEFT, 0, y, &wx, &wy);
        if (wy > wh) break;
        char num[16]; g_snprintf(num, sizeof num, "%d", line + 1);
        pango_layout_set_text(lay, num, -1);
        int pw, ph;
        pango_layout_get_pixel_size(lay, &pw, &ph);
        if (line == cur_line) cairo_set_source_rgb(cr, 0.0, 0.9, 0.63); else cairo_set_source_rgb(cr, 0.30, 0.45, 0.62);
        cairo_move_to(cr, ww - pw - 8, wy);
        pango_cairo_show_layout(cr, lay);
        if (!gtk_text_iter_forward_line(&it)) break;
    }
    g_object_unref(lay);
    return FALSE;
}

static void edit_open(Term *t, const char *arg, const char *orig)
{
    char *p = g_strstrip(g_strdup(arg));
    GError *ue = NULL;
    char *unq = g_shell_unquote(p, &ue);
    if (unq) { g_free(p); p = unq; } else g_clear_error(&ue);
    if (!*p) {
        term_append(t, "usage: edit <file>\n", "err");
        g_free(p); record(t, orig, 2); show_prompt(t); return;
    }
    char *full = p[0] == '~' && (p[1] == '/' || !p[1]) ? g_build_filename(t->app->home, p + (p[1] ? 2 : 1), NULL)
                                                         : g_strdup(p);
    char *path = g_canonicalize_filename(full, t->cwd);
    g_free(full); g_free(p);

    char *contents = NULL; gsize len = 0; gboolean is_new = FALSE;
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        term_appendf(t, "err", "edit: %s is a directory\n", path);
        g_free(path); record(t, orig, 1); show_prompt(t); return;
    }
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        GError *err = NULL;
        if (!g_file_get_contents(path, &contents, &len, &err)) {
            term_appendf(t, "err", "edit: %s\n", err->message);
            g_error_free(err); g_free(path); record(t, orig, 1); show_prompt(t); return;
        }
        if (len > 4 * 1024 * 1024 || !g_utf8_validate(contents, (gssize)len, NULL)) {
            term_appendf(t, "err", "edit: %s is binary or too large for the built-in editor\n", path);
            g_free(contents); g_free(path); record(t, orig, 1); show_prompt(t); return;
        }
    } else {
        is_new = TRUE;
        contents = g_strdup("");
    }

    const char *dot = strrchr(path, '.');
    char *base = g_path_get_basename(path);
    t->ed_mode = 0;
    if (dot && (!strcmp(dot, ".c") || !strcmp(dot, ".h") || !strcmp(dot, ".cpp") || !strcmp(dot, ".cc") ||
                !strcmp(dot, ".hpp") || !strcmp(dot, ".java") || !strcmp(dot, ".js") || !strcmp(dot, ".ts") ||
                !strcmp(dot, ".go") || !strcmp(dot, ".rs") || !strcmp(dot, ".cs")))
        t->ed_mode = 1;
    else if ((dot && (!strcmp(dot, ".py") || !strcmp(dot, ".sh") || !strcmp(dot, ".rb") || !strcmp(dot, ".yml") ||
                      !strcmp(dot, ".yaml") || !strcmp(dot, ".toml") || !strcmp(dot, ".mk"))) || !strcmp(base, "Makefile"))
        t->ed_mode = 2;
    g_free(base);

    g_free(t->ed_path);
    t->ed_path = path;
    t->ed_new = is_new;
    t->ed_loading = TRUE;
    gtk_text_buffer_set_text(t->ed_buf, contents, (gint)len);
    t->ed_loading = FALSE;
    g_free(contents);
    GtkTextIter start;
    gtk_text_buffer_get_start_iter(t->ed_buf, &start);
    gtk_text_buffer_place_cursor(t->ed_buf, &start);
    t->ed_dirty = FALSE;
    ed_highlight(t);

    t->editing = TRUE; t->running = TRUE;
    g_free(t->pending); t->pending = g_strdup(orig);
    ed_update_title(t);
    ed_update_info(t, is_new ? "new file — Ctrl+S creates it" : "");
    gtk_widget_show(t->ed_tv);
    gtk_stack_set_visible_child_name(GTK_STACK(t->stack), "edit");
    gtk_widget_grab_focus(t->ed_tv);
    update_status(t->app);
}

/* ===================================================================== */
/*  Built-ins and dispatch                                                */
/* ===================================================================== */

static int builtin_cd(Term *t, const char *arg)
{
    char *p = g_strstrip(g_strdup(arg));
    GError *ue = NULL;
    char *unq = g_shell_unquote(p, &ue);                  /* handles 'my dir' and "my dir" */
    if (unq) { g_free(p); p = unq; } else g_clear_error(&ue);
    const char *home = t->app->home;
    char *target;
    if (!*p || !strcmp(p, "~"))     target = g_strdup(home);
    else if (!strcmp(p, "-"))       target = g_strdup(t->prev_cwd);
    else if (p[0] == '~' && p[1] == '/') target = g_build_filename(home, p + 2, NULL);
    else                            target = g_strdup(p);
    char *abs = g_canonicalize_filename(target, t->cwd);
    int rc = 1;
    if (g_file_test(abs, G_FILE_TEST_IS_DIR) && access(abs, X_OK) == 0) {
        g_strlcpy(t->prev_cwd, t->cwd, sizeof t->prev_cwd);
        g_strlcpy(t->cwd, abs, sizeof t->cwd);
        rc = 0;
    } else {
        term_appendf(t, "err", "cd: %s: no such directory or permission denied\n", p);
    }
    g_free(p); g_free(target); g_free(abs);
    return rc;
}

static void print_help(Term *t)
{
    term_append(t,
        "hterm built-ins:\n"
        "  cd [dir]        change directory (supports ~, -, quoted paths with spaces)\n"
        "  history         show saved commands     history -c   wipe history + log\n"
        "  clear           clear the screen (Ctrl+L)\n"
        "  edit <file>     built-in code editor (line numbers, colours, auto-indent)\n"
        "                  Ctrl+S save · Esc close · Ctrl+Q discard & close   (alias: e <file>)\n"
        "  help            this message            exit         close hterm\n"
        "Editors & full-screen programs (vi, vim, nano, less, man, top, htop, ssh, python, bash ...)\n"
        "open in a real terminal inside the pane. Use 'pty <cmd>' to force that mode for any command.\n"
        "Panes: Cmd+D split right · Cmd+Shift+D split below · Cmd+W close · Cmd+[ / Cmd+] switch\n"
        "       (Ctrl+Shift+D / Ctrl+Shift+R / Ctrl+Shift+W also work) — or use the buttons on top.\n"
        "Everything else runs through /bin/sh in the current directory.\n", "info");
}

static void run_command(Term *t, const char *raw)
{
    char *line = g_strstrip(g_strdup(raw));
    if (!*line) { g_free(line); show_prompt(t); return; }
    App *a = t->app;

    if (!strcmp(line, "exit") || !strcmp(line, "quit")) {
        record(t, line, 0); g_free(line); gtk_main_quit(); return;
    }
    if (!strcmp(line, "clear")) {
        gtk_text_buffer_set_text(t->buf, "", 0);
        record(t, line, 0);
    } else if (!strncmp(line, "cd", 2) && (line[2] == '\0' || g_ascii_isspace(line[2]))) {
        record(t, line, builtin_cd(t, line + 2));
    } else if (!strcmp(line, "history")) {
        for (size_t i = 0; i < a->hist.count; i++)
            term_appendf(t, "out", "%5zu  %s\n", i + 1, a->hist.e[i].cmd);
        record(t, line, 0);
    } else if (!strcmp(line, "history -c")) {
        hist_clear(&a->hist);
        term_append(t, "history cleared\n", "info");
    } else if (!strcmp(line, "help")) {
        print_help(t); record(t, line, 0);
    } else if (!strncmp(line, "edit", 4) && (line[4] == '\0' || g_ascii_isspace(line[4]))) {
        edit_open(t, line + 4, line);                      /* built-in editor */
        g_free(line);
        return;
    } else if (line[0] == 'e' && g_ascii_isspace(line[1])) {
        edit_open(t, line + 2, line);
        g_free(line);
        return;
    } else {
        const char *cmd = line;
        gboolean forced = FALSE;
        if (g_str_has_prefix(line, "pty ")) { cmd = g_strstrip(line + 4); forced = TRUE; }
        if (*cmd && (forced || needs_tty(cmd))) pty_start(t, cmd, line);
        else spawn_command(t, line);
        g_free(line);
        return;                                  /* prompt returns when the command ends */
    }
    g_free(line);
    show_prompt(t);
}

static void exec_line(Term *t, const char *cmd)
{
    char *draft = get_input(t);
    set_input(t, cmd);
    char *line = get_input(t);
    term_append(t, "\n", NULL);
    run_command(t, line);
    if (!t->running) set_input(t, draft);          /* give back what the user was typing */
    g_free(line); g_free(draft);
}

/* ===================================================================== */
/*  Keyboard handling (line mode)                                         */
/* ===================================================================== */

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w;
    Term *t = d;
    App *a = t->app;
    guint k = ev->keyval;
    gboolean ctrl = (ev->state & GDK_CONTROL_MASK) != 0;
    gboolean is_c = (k == GDK_KEY_c || k == GDK_KEY_C);

    if (t->running) {
        if (t->vt) {                                    /* focus still on the text view: forward to the program */
            char out[32];
            int n = encode_key(t->vt, ev, out);
            if (n > 0) vt_write(t->vt, out, (size_t)n);
            return TRUE;
        }
        if (ctrl && is_c && t->child > 0) kill(-t->child, SIGINT);   /* line-mode command: Ctrl+C only */
        return TRUE;
    }
    if ((ev->state & GDK_MOD1_MASK) && k == GDK_KEY_Up) {            /* Alt+Up: parent dir */
        char *par = g_path_get_dirname(t->cwd);
        navigate_to(a, par);
        g_free(par);
        return TRUE;
    }
    if (ctrl && is_c) {
        if (gtk_text_buffer_get_has_selection(t->buf)) return FALSE;   /* normal copy */
        term_append(t, "^C\n", "info");
        show_prompt(t);
        return TRUE;
    }

    GtkTextIter ins, mk;
    gtk_text_buffer_get_iter_at_mark(t->buf, &ins, gtk_text_buffer_get_insert(t->buf));
    gtk_text_buffer_get_iter_at_mark(t->buf, &mk, t->input_mark);
    if (gtk_text_iter_compare(&ins, &mk) < 0 && !ev->is_modifier) {    /* never edit old output */
        GtkTextIter end; gtk_text_buffer_get_end_iter(t->buf, &end);
        gtk_text_buffer_place_cursor(t->buf, &end);
        ins = end;
    }

    switch (k) {
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: {
        char *line = get_input(t);
        term_append(t, "\n", NULL);
        run_command(t, line);
        g_free(line);
        return TRUE;
    }
    case GDK_KEY_BackSpace: case GDK_KEY_Left:
        return gtk_text_iter_compare(&ins, &mk) <= 0;          /* block at prompt boundary */
    case GDK_KEY_Home: case GDK_KEY_KP_Home:
        gtk_text_buffer_place_cursor(t->buf, &mk);
        return TRUE;
    case GDK_KEY_Up: {
        char *cur = get_input(t);
        const char *h = hist_prev(&a->hist, cur);
        if (h) set_input(t, h);
        g_free(cur);
        return TRUE;
    }
    case GDK_KEY_Down: {
        const char *h = hist_next(&a->hist);
        if (h) set_input(t, h);
        return TRUE;
    }
    case GDK_KEY_Tab: return TRUE;
    default: break;
    }
    if (ctrl && (k == GDK_KEY_l || k == GDK_KEY_L)) {
        gtk_text_buffer_set_text(t->buf, "", 0);
        show_prompt(t);
        return TRUE;
    }
    if (ctrl && (k == GDK_KEY_u || k == GDK_KEY_U)) { set_input(t, ""); return TRUE; }
    return FALSE;
}

/* ===================================================================== */
/*  Panes: create / split / close / focus                                 */
/* ===================================================================== */

static void setup_tags(GtkTextBuffer *b)
{
    gtk_text_buffer_create_tag(b, "prompt",     "foreground", "#00ff9c", "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(b, "prompt_sep", "foreground", "#5b7fa0", NULL);
    gtk_text_buffer_create_tag(b, "prompt_dir", "foreground", "#3aa0ff", "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(b, "prompt_git", "foreground", "#c792ea", NULL);
    gtk_text_buffer_create_tag(b, "out",        "foreground", "#b9e9d8", NULL);
    gtk_text_buffer_create_tag(b, "err",        "foreground", "#ff5c7a", NULL);
    gtk_text_buffer_create_tag(b, "info",       "foreground", "#5aa9ff", NULL);
}

static void set_current(App *a, Term *t)
{
    if (a->cur == t) return;
    a->cur = t;
    for (GList *l = a->terms; l; l = l->next) {
        Term *o = l->data;
        GtkStyleContext *sc = gtk_widget_get_style_context(o->root);
        if (o == t) gtk_style_context_add_class(sc, "pane-active");
        else        gtk_style_context_remove_class(sc, "pane-active");
    }
    refresh_git(a);
    refresh_explorer(a);
    update_status(a);
}

static gboolean on_term_focus(GtkWidget *w, GdkEventFocus *ev, gpointer d)
{
    (void)w; (void)ev;
    Term *t = d;
    if (!t->closed) set_current(t->app, t);
    return FALSE;
}

static Term *term_new(App *a, const char *cwd, gboolean banner)
{
    Term *t = g_new0(Term, 1);
    t->app = a;
    g_strlcpy(t->cwd, cwd, sizeof t->cwd);
    g_strlcpy(t->prev_cwd, cwd, sizeof t->prev_cwd);

    t->buf = gtk_text_buffer_new(NULL);
    setup_tags(t->buf);
    t->tv = gtk_text_view_new_with_buffer(t->buf);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(t->tv), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(t->tv), 16);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(t->tv), 16);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(t->tv), 12);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(t->tv), TRUE);
    GtkTextIter it; gtk_text_buffer_get_end_iter(t->buf, &it);
    t->input_mark = gtk_text_buffer_create_mark(t->buf, "input", &it, TRUE);   /* left gravity */
    t->end_mark   = gtk_text_buffer_create_mark(t->buf, "end",   &it, FALSE);
    g_signal_connect(t->tv, "key-press-event", G_CALLBACK(on_key), t);
    g_signal_connect(t->tv, "focus-in-event", G_CALLBACK(on_term_focus), t);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), t->tv);

    t->vtw = gtk_drawing_area_new();                                          /* real-terminal view */
    gtk_widget_set_can_focus(t->vtw, TRUE);
    gtk_widget_add_events(t->vtw, GDK_KEY_PRESS_MASK | GDK_FOCUS_CHANGE_MASK | GDK_BUTTON_PRESS_MASK);
    g_signal_connect(t->vtw, "draw", G_CALLBACK(draw_vt), t);
    g_signal_connect(t->vtw, "key-press-event", G_CALLBACK(on_vt_key), t);
    g_signal_connect(t->vtw, "focus-in-event", G_CALLBACK(on_vt_focus), t);
    g_signal_connect(t->vtw, "focus-out-event", G_CALLBACK(on_vt_focus), t);
    g_signal_connect(t->vtw, "size-allocate", G_CALLBACK(on_vt_size), t);
    g_signal_connect(t->vtw, "button-press-event", G_CALLBACK(on_vt_click), t);

    /* built-in editor page */
    t->ed_buf = gtk_text_buffer_new(NULL);
    ed_tags(t->ed_buf);
    t->ed_tv = gtk_text_view_new_with_buffer(t->ed_buf);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(t->ed_tv), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(t->ed_tv), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(t->ed_tv), 6);
    gtk_text_view_set_border_window_size(GTK_TEXT_VIEW(t->ed_tv), GTK_TEXT_WINDOW_LEFT, 56);
    g_signal_connect(t->ed_tv, "key-press-event", G_CALLBACK(on_ed_key), t);
    g_signal_connect(t->ed_tv, "draw", G_CALLBACK(on_ed_draw), t);
    g_signal_connect(t->ed_tv, "focus-in-event", G_CALLBACK(on_term_focus), t);
    g_signal_connect(t->ed_buf, "changed", G_CALLBACK(on_ed_changed), t);
    g_signal_connect(t->ed_buf, "notify::cursor-position", G_CALLBACK(on_ed_cursor), t);
    GtkWidget *edsw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(edsw), t->ed_tv);
    GtkWidget *edhdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(edhdr), "edhdr");
    t->ed_title = gtk_label_new("");
    t->ed_info = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(t->ed_title), "pct");
    gtk_style_context_add_class(gtk_widget_get_style_context(t->ed_info), "dim");
    gtk_box_pack_start(GTK_BOX(edhdr), t->ed_title, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(edhdr), t->ed_info, FALSE, FALSE, 0);
    GtkWidget *edbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(edbox), edhdr, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(edbox), edsw, TRUE, TRUE, 0);

    t->stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(t->stack), sw, "text");
    gtk_stack_add_named(GTK_STACK(t->stack), t->vtw, "vt");
    gtk_stack_add_named(GTK_STACK(t->stack), edbox, "edit");
    t->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(t->root), "pane");
    gtk_box_pack_start(GTK_BOX(t->root), t->stack, TRUE, TRUE, 0);
    a->terms = g_list_append(a->terms, t);

    if (banner)
        term_appendf(t, "info", "hterm 0.2 — type 'help'. %zu commands restored from ~/%s/%s\n\n",
                     a->hist.count, HIST_DIR_NAME, HIST_FILE_NAME);
    show_prompt(t);
    return t;
}

static gboolean paned_half_idle(gpointer d)
{
    GtkWidget *p = d;
    gboolean horiz = gtk_orientable_get_orientation(GTK_ORIENTABLE(p)) == GTK_ORIENTATION_HORIZONTAL;
    int size = horiz ? gtk_widget_get_allocated_width(p) : gtk_widget_get_allocated_height(p);
    if (size > 40) gtk_paned_set_position(GTK_PANED(p), size / 2);
    g_object_unref(p);
    return G_SOURCE_REMOVE;
}

/* Replace `old` (a direct child of `parent`) by `repl`, keeping its slot. */
static void put_in_slot(GtkWidget *parent, gboolean first, GtkWidget *repl)
{
    if (GTK_IS_PANED(parent)) {
        if (first) gtk_paned_pack1(GTK_PANED(parent), repl, TRUE, FALSE);
        else       gtk_paned_pack2(GTK_PANED(parent), repl, TRUE, FALSE);
    } else {
        gtk_box_pack_start(GTK_BOX(parent), repl, TRUE, TRUE, 0);
    }
}

static void split_pane(App *a, GtkOrientation orient)
{
    Term *t = a->cur;
    if (!t) return;
    Term *nt = term_new(a, t->cwd, FALSE);                 /* new pane starts in the same directory */

    GtkWidget *parent = gtk_widget_get_parent(t->root);
    gboolean first = GTK_IS_PANED(parent) && gtk_paned_get_child1(GTK_PANED(parent)) == t->root;
    g_object_ref(t->root);
    gtk_container_remove(GTK_CONTAINER(parent), t->root);
    GtkWidget *pn = gtk_paned_new(orient);
    gtk_paned_pack1(GTK_PANED(pn), t->root, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(pn), nt->root, TRUE, FALSE);
    g_object_unref(t->root);
    put_in_slot(parent, first, pn);
    gtk_widget_show_all(pn);
    g_idle_add(paned_half_idle, g_object_ref(pn));         /* 50/50 once it has a size */
    gtk_widget_grab_focus(nt->tv);
    set_current(a, nt);
}

static Term *first_term_under(App *a, GtkWidget *w)
{
    for (GList *l = a->terms; l; l = l->next) {
        Term *t = l->data;
        if (t->root == w || gtk_widget_is_ancestor(t->root, w)) return t;
    }
    return NULL;
}

static void close_pane(App *a, Term *t)
{
    if (!t || g_list_length(a->terms) <= 1) return;        /* keep at least one pane */
    GtkWidget *p = gtk_widget_get_parent(t->root);
    if (!GTK_IS_PANED(p)) return;
    GtkWidget *sib = gtk_paned_get_child1(GTK_PANED(p)) == t->root
                     ? gtk_paned_get_child2(GTK_PANED(p)) : gtk_paned_get_child1(GTK_PANED(p));

    t->closed = TRUE;                                       /* callbacks of this pane become no-ops */
    if (t->vt) vt_hangup(t->vt);
    else if (t->running && t->child > 0) kill(-t->child, SIGTERM);
    a->terms = g_list_remove(a->terms, t);

    GtkWidget *gp = gtk_widget_get_parent(p);
    gboolean first = GTK_IS_PANED(gp) && gtk_paned_get_child1(GTK_PANED(gp)) == p;
    g_object_ref(sib);
    gtk_container_remove(GTK_CONTAINER(p), sib);
    gtk_container_remove(GTK_CONTAINER(gp), p);
    put_in_slot(gp, first, sib);
    g_object_unref(sib);
    gtk_widget_destroy(p);                                  /* destroys the closed pane's widgets */
    gtk_widget_show_all(gp);

    Term *next = first_term_under(a, sib);
    if (!next) next = a->terms->data;
    a->cur = NULL;                                          /* force set_current to refresh everything */
    set_current(a, next);
    focus_term(next);
}

static void cycle_pane(App *a, int dir)
{
    guint n = g_list_length(a->terms);
    if (n < 2 || !a->cur) return;
    int idx = g_list_index(a->terms, a->cur);
    idx = (int)(((unsigned)(idx + dir + (int)n)) % n);
    Term *t = g_list_nth_data(a->terms, (guint)idx);
    focus_term(t);
}

static gboolean on_window_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w;
    App *a = d;
    guint k = ev->keyval;
    gboolean cmd = (ev->state & CMD_MASK) != 0, ctrl = (ev->state & GDK_CONTROL_MASK) != 0,
             shift = (ev->state & GDK_SHIFT_MASK) != 0;
    if (!(cmd || (ctrl && shift))) return FALSE;

    if (k == GDK_KEY_d || k == GDK_KEY_D) {
        if (shift) split_pane(a, GTK_ORIENTATION_VERTICAL);         /* ⌘⇧D: new pane below (iTerm) */
        else       split_pane(a, GTK_ORIENTATION_HORIZONTAL);       /* ⌘D : new pane to the right  */
        return TRUE;
    }
    if (ctrl && shift && (k == GDK_KEY_r || k == GDK_KEY_R)) { split_pane(a, GTK_ORIENTATION_HORIZONTAL); return TRUE; }
    if (k == GDK_KEY_w || k == GDK_KEY_W) { close_pane(a, a->cur); return TRUE; }
    if (cmd && (k == GDK_KEY_bracketright || k == GDK_KEY_braceright)) { cycle_pane(a, 1); return TRUE; }
    if (cmd && (k == GDK_KEY_bracketleft || k == GDK_KEY_braceleft)) { cycle_pane(a, -1); return TRUE; }
    return FALSE;
}

static void on_btn_split_right(GtkButton *b, gpointer d) { (void)b; split_pane(d, GTK_ORIENTATION_HORIZONTAL); }
static void on_btn_split_below(GtkButton *b, gpointer d) { (void)b; split_pane(d, GTK_ORIENTATION_VERTICAL); }
static void on_btn_close(GtkButton *b, gpointer d) { (void)b; App *a = d; close_pane(a, a->cur); }

static GtkWidget *label_new(const char *txt, const char *cls)
{
    GtkWidget *l = gtk_label_new(txt);
    if (cls) gtk_style_context_add_class(gtk_widget_get_style_context(l), cls);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    return l;
}

static GtkWidget *ellipsized(const char *cls)
{
    GtkWidget *l = label_new("", cls);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 24);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    return l;
}

/* ===================================================================== */
/*  Git information (all values come from the real repository)            */
/* ===================================================================== */

static gchar *git_run(const char *cwd, ...) G_GNUC_NULL_TERMINATED;
static gchar *git_run(const char *cwd, ...)
{
    GPtrArray *v = g_ptr_array_new();
    g_ptr_array_add(v, (gpointer)"git"); g_ptr_array_add(v, (gpointer)"-C"); g_ptr_array_add(v, (gpointer)cwd);
    va_list ap; va_start(ap, cwd);
    const char *s;
    while ((s = va_arg(ap, const char *))) g_ptr_array_add(v, (gpointer)s);
    va_end(ap);
    g_ptr_array_add(v, NULL);
    gchar *out = NULL; gint st = 0;
    gboolean ok = g_spawn_sync(NULL, (gchar **)v->pdata, NULL,
                               G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                               NULL, NULL, &out, NULL, &st, NULL);
    g_ptr_array_free(v, TRUE);
    if (!ok || st != 0) { g_free(out); return NULL; }
    return out ? out : g_strdup("");
}

/* git@github.com:me/x.git, ssh://git@host:22/me/x.git, https://tok@host/me/x.git → https://host/me/x */
static void remote_to_web(const char *url, char *out, size_t n)
{
    out[0] = '\0';
    char *t = g_strdup(url);
    size_t l = strlen(t);
    while (l && t[l - 1] == '/') t[--l] = '\0';
    if (l > 4 && !strcmp(t + l - 4, ".git")) t[l - 4] = '\0';

    if (g_str_has_prefix(t, "https://") || g_str_has_prefix(t, "http://")) {
        char *scheme_end = strstr(t, "://") + 3;
        char *slash = strchr(scheme_end, '/'), *at = strchr(scheme_end, '@');
        if (at && (!slash || at < slash)) scheme_end = at + 1;               /* drop credentials */
        g_snprintf(out, n, "https://%s", scheme_end);
    } else if (g_str_has_prefix(t, "ssh://")) {
        char *host = t + 6, *at = strchr(host, '@');
        if (at) host = at + 1;
        char *slash = strchr(host, '/');
        if (slash) { *slash = '\0'; char *colon = strchr(host, ':'); if (colon) *colon = '\0';
                     g_snprintf(out, n, "https://%s/%s", host, slash + 1); }
    } else if (strchr(t, '@') && strchr(t, ':') && !strstr(t, "://")) {
        char *host = strchr(t, '@') + 1, *colon = strchr(host, ':');
        if (colon) { *colon = '\0'; g_snprintf(out, n, "https://%s/%s", host, colon + 1); }
    }
    g_free(t);
}

static void update_git_widgets(App *a)
{
    GitInfo *g = &a->git; char *m;
    if (!g->is_repo) {
        gtk_label_set_markup(GTK_LABEL(a->gi_repo), "<span foreground='#ff5c7a'>Not a git repository</span>");
        gtk_label_set_markup(GTK_LABEL(a->gi_branch), "<span foreground='#5b7fa0'>run  git init  to start one</span>");
        gtk_label_set_text(GTK_LABEL(a->gi_remote), ""); gtk_label_set_text(GTK_LABEL(a->gi_url), "");
        gtk_label_set_text(GTK_LABEL(a->gi_status), ""); gtk_label_set_text(GTK_LABEL(a->gi_last), "");
        gtk_widget_set_sensitive(a->gi_open, FALSE);
        return;
    }
    m = g_markup_printf_escaped("<span foreground='#5b7fa0'>Repo:</span>   %s", g->name);
    gtk_label_set_markup(GTK_LABEL(a->gi_repo), m); g_free(m);

    if (g->detached) m = g_markup_printf_escaped("<span foreground='#5b7fa0'>Branch:</span> <span foreground='#ffb454'>HEAD detached (%s)</span>", g->branch);
    else if (g->has_upstream) m = g_markup_printf_escaped("<span foreground='#5b7fa0'>Branch:</span> %s  <span foreground='#00ff9c'>↑%d</span> <span foreground='#ff5c7a'>↓%d</span>", g->branch, g->ahead, g->behind);
    else m = g_markup_printf_escaped("<span foreground='#5b7fa0'>Branch:</span> %s  <span foreground='#5b7fa0'>(no upstream)</span>", g->branch);
    gtk_label_set_markup(GTK_LABEL(a->gi_branch), m); g_free(m);

    if (g->remote[0]) m = g_markup_printf_escaped("<span foreground='#5b7fa0'>Remote:</span> %s", g->remote);
    else m = g_markup_printf_escaped("<span foreground='#5b7fa0'>Remote:</span> <span foreground='#ffb454'>none configured</span>");
    gtk_label_set_markup(GTK_LABEL(a->gi_remote), m); g_free(m);

    const char *show = g->web[0] ? g->web + (g_str_has_prefix(g->web, "https://") ? 8 : 7)
                                 : (strchr(g->url, '@') ? "" : g->url);      /* never show credentials */
    m = g_markup_printf_escaped("<span foreground='#5b7fa0'>URL:</span>    %s", show);
    gtk_label_set_markup(GTK_LABEL(a->gi_url), m); g_free(m);

    if (!g->mod && !g->staged && !g->untracked)
        m = g_strdup("<span foreground='#5b7fa0'>Status:</span> <span foreground='#00ff9c'>clean ✓</span>");
    else m = g_markup_printf_escaped("<span foreground='#5b7fa0'>Status:</span> <span foreground='#ffb454'>%d mod</span> · <span foreground='#00ff9c'>%d staged</span> · <span foreground='#3aa0ff'>%d new</span>",
                                     g->mod, g->staged, g->untracked);
    gtk_label_set_markup(GTK_LABEL(a->gi_status), m); g_free(m);

    m = g_markup_printf_escaped("<span foreground='#5b7fa0'>Last:</span>   %s", g->last[0] ? g->last : "no commits yet");
    gtk_label_set_markup(GTK_LABEL(a->gi_last), m); g_free(m);
    gtk_widget_set_sensitive(a->gi_open, g->web[0] != '\0');
}

static void refresh_git(App *a)
{
    GitInfo *g = &a->git;
    memset(g, 0, sizeof *g);
    gchar *top = git_run(a->cur->cwd, "rev-parse", "--show-toplevel", NULL);
    if (!top) { update_git_widgets(a); return; }                  /* not a repository */
    g->is_repo = TRUE;
    g_strstrip(top);
    char *bn = g_path_get_basename(top);
    g_strlcpy(g->name, bn, sizeof g->name);
    g_free(bn); g_free(top);

    char oid[16] = "";
    gchar *st = git_run(a->cur->cwd, "status", "--porcelain=v2", "--branch", NULL);
    if (st) {
        gchar **ln = g_strsplit(st, "\n", -1);
        for (int i = 0; ln[i]; i++) {
            const char *l = ln[i];
            if (g_str_has_prefix(l, "# branch.head ")) {
                g_strlcpy(g->branch, l + 14, sizeof g->branch);
                if (!strcmp(g->branch, "(detached)")) g->detached = TRUE;
            } else if (g_str_has_prefix(l, "# branch.oid ")) {
                g_strlcpy(oid, l + 13, 8);
            } else if (g_str_has_prefix(l, "# branch.upstream ")) {
                g->has_upstream = TRUE;
            } else if (g_str_has_prefix(l, "# branch.ab ")) {
                sscanf(l, "# branch.ab +%d -%d", &g->ahead, &g->behind);
            } else if ((l[0] == '1' || l[0] == '2') && strlen(l) > 4) {
                if (l[2] != '.') g->staged++;
                if (l[3] != '.') g->mod++;
            } else if (l[0] == 'u') { g->mod++; }
            else if (l[0] == '?') { g->untracked++; }
        }
        g_strfreev(ln); g_free(st);
    }
    if (g->detached) g_strlcpy(g->branch, oid, sizeof g->branch);

    gchar *rem = git_run(a->cur->cwd, "remote", NULL);
    if (rem) {
        char *nl = strchr(rem, '\n'); if (nl) *nl = '\0';
        g_strstrip(rem);
        g_strlcpy(g->remote, rem, sizeof g->remote);
        g_free(rem);
    }
    if (g->remote[0]) {
        gchar *u = git_run(a->cur->cwd, "remote", "get-url", g->remote, NULL);
        if (u) { g_strstrip(u); g_strlcpy(g->url, u, sizeof g->url); g_free(u);
                 remote_to_web(g->url, g->web, sizeof g->web); }
    }
    gchar *lg = git_run(a->cur->cwd, "log", "-1", "--pretty=format:%h %s", NULL);
    if (lg) { g_strlcpy(g->last, lg, sizeof g->last); g_free(lg); }
    update_git_widgets(a);
}

static void on_open_remote(GtkButton *b, gpointer d)
{
    (void)b;
    App *a = d;
    if (!g_str_has_prefix(a->git.web, "https://") && !g_str_has_prefix(a->git.web, "http://")) return;
#ifdef __APPLE__
    gchar *argv[] = { (gchar *)"open", a->git.web, NULL };
#else
    gchar *argv[] = { (gchar *)"xdg-open", a->git.web, NULL };
#endif
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                  G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL);
}

/* ===================================================================== */
/*  File explorer (current folder + parent/siblings, click to cd)         */
/* ===================================================================== */

enum { C_ICON, C_NAME, C_COUNT, C_PATH, C_ISDIR, C_N };
#define MAX_ENT 300

typedef struct { char *name; gboolean dir; } FsEnt;
typedef struct { App *a; char *path; } NavReq;

static void ent_free(gpointer p) { FsEnt *e = p; g_free(e->name); g_free(e); }

static gint cmp_ent(gconstpointer pa, gconstpointer pb)
{
    const FsEnt *x = *(FsEnt *const *)pa, *y = *(FsEnt *const *)pb;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return g_ascii_strcasecmp(x->name, y->name);
}

static int count_entries(const char *path)
{
    GDir *d = g_dir_open(path, 0, NULL);
    if (!d) return 0;
    int n = 0;
    while (g_dir_read_name(d) && n < 9999) n++;
    g_dir_close(d);
    return n;
}

static GPtrArray *list_dir(const char *path, gboolean dirs_only)
{
    GPtrArray *v = g_ptr_array_new_with_free_func(ent_free);
    GDir *d = g_dir_open(path, 0, NULL);
    if (!d) return v;
    const char *nm; int n = 0;
    while ((nm = g_dir_read_name(d)) && n < MAX_ENT) {
        n++;
        char *full = g_build_filename(path, nm, NULL);
        gboolean isd = g_file_test(full, G_FILE_TEST_IS_DIR);
        g_free(full);
        if (dirs_only && !isd) continue;
        FsEnt *e = g_new0(FsEnt, 1);
        e->name = g_strdup(nm); e->dir = isd;
        g_ptr_array_add(v, e);
    }
    g_dir_close(d);
    g_ptr_array_sort(v, cmp_ent);
    return v;
}

static void add_children(App *a, GtkTreeIter *parent, const char *path)
{
    GPtrArray *v = list_dir(path, FALSE);
    for (guint i = 0; i < v->len; i++) {
        FsEnt *e = g_ptr_array_index(v, i);
        char *full = g_build_filename(path, e->name, NULL);
        char *cnt = e->dir ? g_strdup_printf("(%d)", count_entries(full)) : g_strdup("");
        GtkTreeIter it;
        gtk_tree_store_append(a->cur_store, &it, parent);
        gtk_tree_store_set(a->cur_store, &it, C_ICON, e->dir ? "folder" : "text-x-generic",
                           C_NAME, e->name, C_COUNT, cnt, C_PATH, full, C_ISDIR, e->dir, -1);
        if (e->dir) {                                          /* placeholder → lazy expand */
            GtkTreeIter ph;
            gtk_tree_store_append(a->cur_store, &ph, &it);
            gtk_tree_store_set(a->cur_store, &ph, C_NAME, "…", C_PATH, "", C_ISDIR, FALSE, -1);
        }
        g_free(full); g_free(cnt);
    }
    g_ptr_array_free(v, TRUE);
}

static gboolean on_test_expand(GtkTreeView *tv, GtkTreeIter *iter, GtkTreePath *path, gpointer d)
{
    (void)tv; (void)path;
    App *a = d;
    GtkTreeModel *m = GTK_TREE_MODEL(a->cur_store);
    GtkTreeIter ph;
    if (!gtk_tree_model_iter_children(m, &ph, iter)) return FALSE;
    gchar *cp = NULL, *dp = NULL;
    gtk_tree_model_get(m, &ph, C_PATH, &cp, -1);
    if (cp && !*cp) {
        gtk_tree_model_get(m, iter, C_PATH, &dp, -1);
        GtkTreeIter ph2 = ph;
        if (dp) add_children(a, iter, dp);
        gtk_tree_store_remove(a->cur_store, &ph2);
    }
    g_free(cp); g_free(dp);
    return FALSE;
}

static void refresh_explorer(App *a)
{
    gtk_label_set_text(GTK_LABEL(a->ex_path), a->cur->cwd);
    gtk_tree_store_clear(a->cur_store);
    add_children(a, NULL, a->cur->cwd);
    char *base = g_path_get_basename(a->cur->cwd), *cnt = g_strdup_printf("(%d)", count_entries(a->cur->cwd));
    char *nm = g_strdup_printf("▾ %s", base);
    gtk_label_set_text(GTK_LABEL(a->cur_name), nm);
    gtk_label_set_text(GTK_LABEL(a->cur_cnt), cnt);
    g_free(base); g_free(cnt); g_free(nm);

    char *par = g_path_get_dirname(a->cur->cwd);
    gboolean root = strcmp(par, a->cur->cwd) == 0;
    g_free(a->par_path);
    a->par_path = root ? NULL : g_strdup(par);
    char *pl = g_strdup_printf("↰ %s", root ? "/ (root)" : par);
    gtk_label_set_text(GTK_LABEL(a->par_lbl), pl);
    g_free(pl);

    gtk_list_store_clear(a->par_store);
    if (!root) {
        GPtrArray *v = list_dir(par, TRUE);
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(a->par_tv));
        for (guint i = 0; i < v->len; i++) {
            FsEnt *e = g_ptr_array_index(v, i);
            char *full = g_build_filename(par, e->name, NULL);
            char *c = g_strdup_printf("(%d)", count_entries(full));
            GtkTreeIter it;
            gtk_list_store_append(a->par_store, &it);
            gtk_list_store_set(a->par_store, &it, C_ICON, "folder", C_NAME, e->name, C_COUNT, c,
                               C_PATH, full, C_ISDIR, TRUE, -1);
            if (!strcmp(full, a->cur->cwd)) gtk_tree_selection_select_iter(sel, &it);
            g_free(full); g_free(c);
        }
        g_ptr_array_free(v, TRUE);
    }
    g_free(par);
}

static gboolean nav_idle(gpointer d)
{
    NavReq *r = d;
    navigate_to(r->a, r->path);
    g_free(r->path); g_free(r);
    return G_SOURCE_REMOVE;
}

static void queue_nav(App *a, const char *path)
{
    NavReq *r = g_new0(NavReq, 1);                  /* deferred: the tree model is rebuilt by the cd */
    r->a = a; r->path = g_strdup(path);
    g_idle_add(nav_idle, r);
}

static void navigate_to(App *a, const char *path)
{
    if (a->cur->running) {
        gtk_label_set_text(GTK_LABEL(a->ex_help), "a command is running — press Ctrl+C first");
        return;
    }
    char *q = g_shell_quote(path);                  /* spaces / quotes in names are safe */
    char *cmd = g_strdup_printf("cd %s", q);
    exec_line(a->cur, cmd);
    char *h = g_strdup_printf("→ %s", cmd);
    gtk_label_set_text(GTK_LABEL(a->ex_help), h);
    g_free(h); g_free(cmd); g_free(q);
    focus_term(a->cur);
}

static gboolean on_tree_click(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
    App *a = d;
    if (ev->button != 1 || ev->type != GDK_BUTTON_PRESS) return FALSE;
    GtkTreeView *tv = GTK_TREE_VIEW(w);
    GtkTreePath *p = NULL; GtkTreeViewColumn *col = NULL;
    if (!gtk_tree_view_get_path_at_pos(tv, (gint)ev->x, (gint)ev->y, &p, &col, NULL, NULL)) return FALSE;

    GdkRectangle r; gint bx, by;
    gtk_tree_view_get_cell_area(tv, p, col, &r);
    gtk_tree_view_convert_widget_to_bin_window_coords(tv, (gint)ev->x, (gint)ev->y, &bx, &by);
    if (col == gtk_tree_view_get_column(tv, 0) && bx < r.x) {       /* clicked the expander arrow */
        gtk_tree_path_free(p);
        return FALSE;
    }
    GtkTreeModel *m = gtk_tree_view_get_model(tv);
    GtkTreeIter it; gchar *path = NULL; gboolean isd = FALSE;
    gboolean handled = FALSE;
    if (gtk_tree_model_get_iter(m, &it, p)) {
        gtk_tree_model_get(m, &it, C_PATH, &path, C_ISDIR, &isd, -1);
        if (isd && path && *path) {
            char *q = g_shell_quote(path), *h = g_strdup_printf("→ cd %s", q);
            gtk_label_set_text(GTK_LABEL(a->ex_help), h);
            g_free(q); g_free(h);
            queue_nav(a, path);
            handled = TRUE;
        }
    }
    g_free(path);
    gtk_tree_path_free(p);
    return handled;
}

static void on_row_activated(GtkTreeView *tv, GtkTreePath *p, GtkTreeViewColumn *c, gpointer d)
{
    (void)c;
    GtkTreeModel *m = gtk_tree_view_get_model(tv);
    GtkTreeIter it; gchar *path = NULL; gboolean isd = FALSE;
    if (!gtk_tree_model_get_iter(m, &it, p)) return;
    gtk_tree_model_get(m, &it, C_PATH, &path, C_ISDIR, &isd, -1);
    if (isd && path && *path) queue_nav(d, path);
    g_free(path);
}

static gboolean on_parent_click(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
    (void)w;
    App *a = d;
    if (ev->button == 1 && ev->type == GDK_BUTTON_PRESS && a->par_path) queue_nav(a, a->par_path);
    return TRUE;
}

static GtkWidget *tree_new(GtkTreeModel *model, GtkWidget **tv_out, App *a)
{
    GtkWidget *tv = gtk_tree_view_new_with_model(model);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tv), FALSE);
    gtk_tree_view_set_enable_tree_lines(GTK_TREE_VIEW(tv), TRUE);

    GtkTreeViewColumn *c = gtk_tree_view_column_new();
    GtkCellRenderer *ri = gtk_cell_renderer_pixbuf_new(), *rt = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(c, ri, FALSE);
    gtk_tree_view_column_add_attribute(c, ri, "icon-name", C_ICON);
    gtk_tree_view_column_pack_start(c, rt, TRUE);
    gtk_tree_view_column_add_attribute(c, rt, "text", C_NAME);
    gtk_tree_view_column_set_expand(c, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv), c);

    GtkCellRenderer *rc = gtk_cell_renderer_text_new();
    g_object_set(rc, "xalign", 1.0, "foreground", "#5b7fa0", NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv),
        gtk_tree_view_column_new_with_attributes("", rc, "text", C_COUNT, NULL));

    g_signal_connect(tv, "button-press-event", G_CALLBACK(on_tree_click), a);
    g_signal_connect(tv, "row-activated", G_CALLBACK(on_row_activated), a);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), 170);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    *tv_out = tv;
    return sw;
}

/* ===================================================================== */
/*  Quick commands                                                        */
/* ===================================================================== */

enum { K_RUN, K_DESTRUCTIVE, K_TEMPLATE, K_TTY, K_INFO };   /* what a double-click may do (K_INFO: explanation only) */

typedef struct { const char *group, *cmd, *desc; int kind; } QCmd;

static const QCmd QCMDS[] = {
    { "C & BUILD", "gcc main.c -o app",           "Compile a C program", K_RUN },
    { "C & BUILD", "./app",                       "Run the compiled program", K_RUN },
    { "C & BUILD", "gcc -Wall -Wextra main.c -o app", "Compile with additional warnings", K_RUN },
    { "C & BUILD", "gcc -g main.c -o app",        "Build with debugging information", K_RUN },
    { "C & BUILD", "make",                        "Build using a Makefile", K_RUN },
    { "C & BUILD", "make clean",                  "Remove generated build files", K_DESTRUCTIVE },
    { "C & BUILD", "rm app",                      "Remove a generated executable", K_DESTRUCTIVE },

    { "NAVIGATION", "pwd",         "Display the current directory", K_RUN },
    { "NAVIGATION", "ls",          "List directory contents", K_RUN },
    { "NAVIGATION", "ls -la",      "Show all files, including hidden files", K_RUN },
    { "NAVIGATION", "cd folder",   "Enter a directory (edit the name first)", K_TEMPLATE },
    { "NAVIGATION", "cd ..",       "Move to the parent directory", K_RUN },
    { "NAVIGATION", "cd ~",        "Return to the home directory", K_RUN },
    { "NAVIGATION", "mkdir folder","Create a directory (edit the name first)", K_TEMPLATE },
    { "NAVIGATION", "touch file.c","Create an empty file (edit the name first)", K_TEMPLATE },

    { "GIT COMMANDS", "git status",                  "Show the working-tree status", K_RUN },
    { "GIT COMMANDS", "git branch",                  "List local branches", K_RUN },
    { "GIT COMMANDS", "git branch --show-current",   "Display the current branch", K_RUN },
    { "GIT COMMANDS", "git switch main",             "Switch branches", K_RUN },
    { "GIT COMMANDS", "git switch -c feature-name",  "Create and switch to a branch (edit the name)", K_TEMPLATE },
    { "GIT COMMANDS", "git add .",                   "Stage changes", K_RUN },
    { "GIT COMMANDS", "git commit -m \"message\"",   "Create a commit (edit the message)", K_TEMPLATE },
    { "GIT COMMANDS", "git log --oneline -5",        "Show recent commits", K_RUN },
    { "GIT COMMANDS", "git pull",                    "Fetch and integrate remote changes", K_RUN },
    { "GIT COMMANDS", "git push",                    "Push commits to the configured remote", K_DESTRUCTIVE },
    { "GIT COMMANDS", "git remote -v",               "Display remote repositories", K_RUN },
    { "GIT COMMANDS", "git diff",                    "Show unstaged changes", K_RUN },
    { "GIT COMMANDS", "git clone <repository-url>",  "Clone a repository (edit the URL)", K_TEMPLATE },

    { "FILE EDITING", "edit demo.c",          "Built-in editor: Ctrl+S save · Esc close (edit the name first)", K_TEMPLATE },
    { "FILE EDITING", "vi demo.c",            "Edit with vi in a real terminal (edit the name first)", K_TEMPLATE },
    { "FILE EDITING", "nano demo.c",          "Edit with nano: Ctrl+O save, Ctrl+X exit (edit the name)", K_TEMPLATE },
    { "FILE EDITING", "touch demo.c",         "Create an empty file (edit the name first)", K_TEMPLATE },
    { "FILE EDITING", "cat demo.c",           "Print a file (edit the name first)", K_TEMPLATE },
    { "FILE EDITING", "less demo.c",          "Page through a file, q quits (edit the name)", K_TEMPLATE },
    { "FILE EDITING", "head -n 20 demo.c",    "Show the first 20 lines of a file", K_TEMPLATE },
    { "FILE EDITING", "tail -n 20 demo.c",    "Show the last 20 lines of a file", K_TEMPLATE },
    { "FILE EDITING", "grep -n main demo.c",  "Search text in a file, with line numbers", K_TEMPLATE },
    { "FILE EDITING", "wc -l demo.c",         "Count the lines in a file", K_TEMPLATE },
    { "FILE EDITING", "cp demo.c backup.c",   "Copy a file (edit the names first)", K_TEMPLATE },
    { "FILE EDITING", "mv demo.c new.c",      "Rename or move a file (edit the names)", K_TEMPLATE },
    { "FILE EDITING", "rm demo.c",            "Delete a file — check the name before pressing Enter", K_TEMPLATE },
    { "FILE EDITING", "diff a.c b.c",         "Compare two files", K_TEMPLATE },

    { "VI KEYS", "i",          "vi: insert mode — start typing text", K_INFO },
    { "VI KEYS", "Esc",        "vi: back to normal mode", K_INFO },
    { "VI KEYS", ":w",         "vi: save the file", K_INFO },
    { "VI KEYS", ":q",         "vi: quit (only if nothing changed)", K_INFO },
    { "VI KEYS", ":wq",        "vi: save and quit", K_INFO },
    { "VI KEYS", ":q!",        "vi: quit and throw changes away", K_INFO },
    { "VI KEYS", "dd",         "vi: delete the current line", K_INFO },
    { "VI KEYS", "yy  then p", "vi: copy the current line, then paste it", K_INFO },
    { "VI KEYS", "u",          "vi: undo the last change", K_INFO },
    { "VI KEYS", "/text",      "vi: search forward, n = next match", K_INFO },
    { "VI KEYS", ":set number","vi: show line numbers", K_INFO },
    { "VI KEYS", "gg / G",     "vi: jump to the first / last line", K_INFO },

    { "SYSTEM UTILITIES", "top",       "View running processes (needs a real TTY)", K_TTY },
    { "SYSTEM UTILITIES", "htop",      "Interactive process monitor (needs a real TTY)", K_TTY },
    { "SYSTEM UTILITIES", "clear",     "Clear the terminal", K_RUN },
    { "SYSTEM UTILITIES", "history",   "View command history", K_RUN },
    { "SYSTEM UTILITIES", "whoami",    "Display the current username", K_RUN },
    { "SYSTEM UTILITIES", "free -h",   "Display memory usage (Linux)", K_RUN },
    { "SYSTEM UTILITIES", "df -h",     "Display disk usage", K_RUN },
    { "SYSTEM UTILITIES", "uname -a",  "Display system information", K_RUN },
};

static void set_cmd_help(App *a, const char *fmt, const char *cmd, const char *desc)
{
    char *t = g_strdup_printf(fmt, cmd, desc);
    gtk_label_set_text(GTK_LABEL(a->cmd_help), t);
    g_free(t);
}

static gboolean confirm_run(App *a, const char *cmd)
{
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(a->win), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                                            GTK_BUTTONS_OK_CANCEL, "Run a potentially destructive command?");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s\n\nin %s", cmd, a->cur->cwd);
    gint r = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    return r == GTK_RESPONSE_OK;
}

static gboolean on_cmd_hover(GtkWidget *w, GdkEventCrossing *ev, gpointer d)
{
    (void)ev;
    const QCmd *q = g_object_get_data(G_OBJECT(w), "qc");
    if (q) set_cmd_help(d, "%s   # %s", q->cmd, q->desc);
    return FALSE;
}

static gboolean on_cmd_click(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
    App *a = d;
    const QCmd *q = g_object_get_data(G_OBJECT(w), "qc");
    if (!q) return FALSE;

    if (ev->button == 3) {                                          /* copy */
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), q->cmd, -1);
        set_cmd_help(a, "✔ copied to clipboard: %s   # %s", q->cmd, q->desc);
        return TRUE;
    }
    if (q->kind == K_INFO) {                                        /* explanation only */
        if (ev->button == 1) set_cmd_help(a, "ℹ %s   # %s", q->cmd, q->desc);
        return TRUE;
    }
    if (ev->button != 1 || a->cur->running) return TRUE;

    if (ev->type == GDK_BUTTON_PRESS) {                             /* insert (never runs) */
        set_input(a->cur, q->cmd);
        focus_term(a->cur);
        set_cmd_help(a, "✔ inserted: %s   · double-click runs · right-click copies   # %s", q->cmd, q->desc);
    } else if (ev->type == GDK_2BUTTON_PRESS) {                     /* run */
        if (q->kind == K_TEMPLATE) {
            set_cmd_help(a, "✎ %s is a template — edit it in the terminal, then press Enter   # %s", q->cmd, q->desc);
        } else if (q->kind == K_TTY) {
            set_cmd_help(a, "✖ %s needs a real TTY — not supported in this build   # %s", q->cmd, q->desc);
        } else if (q->kind == K_DESTRUCTIVE && !confirm_run(a, q->cmd)) {
            set_cmd_help(a, "cancelled: %s   # %s", q->cmd, q->desc);
        } else {
            exec_line(a->cur, q->cmd);
            set_cmd_help(a, "▶ executed: %s   # %s", q->cmd, q->desc);
        }
        focus_term(a->cur);
    }
    return TRUE;
}

static GtkWidget *cmd_group(App *a, const char *group, const char *color)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    char *t = g_markup_printf_escaped("<span foreground='%s' weight='bold'>%s</span>", color, group);
    GtkWidget *title = label_new("", "title");
    gtk_label_set_markup(GTK_LABEL(title), t);
    g_free(t);
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 2);

    for (guint i = 0; i < G_N_ELEMENTS(QCMDS); i++) {
        if (strcmp(QCMDS[i].group, group)) continue;
        GtkWidget *eb = gtk_event_box_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(eb), "cmd");
        char *m = g_markup_printf_escaped("<span foreground='%s'>%s</span>", color, QCMDS[i].cmd);
        GtkWidget *l = label_new("", NULL);
        gtk_label_set_markup(GTK_LABEL(l), m);
        g_free(m);
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 22);
        gtk_container_add(GTK_CONTAINER(eb), l);
        gtk_widget_add_events(eb, GDK_BUTTON_PRESS_MASK | GDK_ENTER_NOTIFY_MASK);
        g_object_set_data(G_OBJECT(eb), "qc", (gpointer)&QCMDS[i]);
        gtk_widget_set_tooltip_text(eb, QCMDS[i].desc);
        g_signal_connect(eb, "button-press-event", G_CALLBACK(on_cmd_click), a);
        g_signal_connect(eb, "enter-notify-event", G_CALLBACK(on_cmd_hover), a);
        gtk_box_pack_start(GTK_BOX(box), eb, FALSE, FALSE, 0);
    }
    return box;
}

/* ===================================================================== */
/*  Animated pixel scenes (64x40 canvas, ~12 fps, auto-rotating)          */
/* ===================================================================== */

enum { SC_FIRE, SC_THUNDER, SC_SEA, SC_AIR, SC_DARK, SC_HORUS, SC_CROSS, SC_PYRAMID, SC_DANCER, SC_RELATIVITY,
       SC_CUSTOM, SC_COUNT };
static const char *const SCENE_NAMES[SC_COUNT] = {
    "FIRE", "THUNDER", "SEA", "AIR", "DARK CLOUDS", "EYE OF HORUS", "CROSS ON THE MOUNTAIN", "PYRAMIDS",
    "STAGE DANCER", "RELATIVITY", "CUSTOM" };

static guint32 RGB(int r, int g, int b)
{
    r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (guint32)(r << 16 | g << 8 | b);
}

static guint32 mixc(guint32 x, guint32 y, double t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int xr = (x >> 16) & 255, xg = (x >> 8) & 255, xb = x & 255, yr = (y >> 16) & 255, yg = (y >> 8) & 255, yb = y & 255;
    return RGB((int)(xr + (yr - xr) * t), (int)(xg + (yg - xg) * t), (int)(xb + (yb - xb) * t));
}

static guint hash2(int x, int y, int s)
{
    guint h = (guint)x * 374761393u + (guint)y * 668265263u + (guint)s * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static void put(App *a, int x, int y, guint32 c) { if (x >= 0 && x < SW && y >= 0 && y < SH) a->px[y * SW + x] = c; }
static void blend(App *a, int x, int y, guint32 c, double t)
{
    if (x >= 0 && x < SW && y >= 0 && y < SH) a->px[y * SW + x] = mixc(a->px[y * SW + x], c, t);
}
static void fillrect(App *a, int x0, int y0, int x1, int y1, guint32 c)
{
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) put(a, x, y, c);
}
static void gradient(App *a, int y0, int y1, guint32 top, guint32 bot)
{
    for (int y = y0; y < y1 && y < SH; y++) {
        double t = y1 - y0 > 1 ? (double)(y - y0) / (y1 - y0 - 1) : 0;
        for (int x = 0; x < SW; x++) put(a, x, y, mixc(top, bot, t));
    }
}
static void disc(App *a, int cx, int cy, int r, guint32 c, double t)
{
    for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++)
        if (dx * dx + dy * dy <= r * r + r / 2) blend(a, cx + dx, cy + dy, c, t);
}
static void line(App *a, int x0, int y0, int x1, int y1, guint32 c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (int guard = 0; guard < 400; guard++) {
        put(a, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
static void cloud(App *a, int cx, int cy, int w, guint32 lit, guint32 shade)
{
    for (int i = 0; i < 5; i++) {
        int bx = cx + (i - 2) * w / 5, r = w / 7 + (i == 2 ? 2 : (i % 2));
        disc(a, bx, cy + 1 + (i % 2), r, shade, 1.0);
    }
    for (int i = 0; i < 5; i++) {
        int bx = cx + (i - 2) * w / 5, r = w / 7 + (i == 2 ? 2 : (i % 2));
        disc(a, bx, cy - (i % 2), r, lit, 1.0);
    }
}
static int pmod(int a, int m) { int r = a % m; return r < 0 ? r + m : r; }

static guint32 heat_color(int h)
{
    double t = h / 36.0;
    int r = (int)((0.25 + t * 1.5) * 255), g = (int)((t - 0.5) * 1.7 * 255), b = (int)((t - 0.92) * 3.0 * 255);
    return RGB(r, g, b);
}

static void scene_fire(App *a)
{
    for (int x = 0; x < SW; x++) {                          /* bonfire-shaped heat source */
        int dist = abs(2 * x - (SW - 1));
        int h = 36 - dist * 36 / 44 + g_random_int_range(-3, 3);
        a->heat[(SH - 1) * SW + x] = (guint8)(h < 0 ? 0 : h > 36 ? 36 : h);
    }
    for (int y = 1; y < SH; y++)
        for (int x = 0; x < SW; x++) {
            int src = a->heat[y * SW + x], r = g_random_int_range(0, 3), dx = x - r + 1;
            if (dx < 0) dx = 0;
            if (dx >= SW) dx = SW - 1;
            int v = src - (r == 1 ? 2 : 1);
            a->heat[(y - 1) * SW + dx] = (guint8)(v < 0 ? 0 : v);
        }
    gradient(a, 0, SH, RGB(14, 4, 12), RGB(40, 8, 8));
    for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) {
        int h = a->heat[y * SW + x];
        if (h >= 3) put(a, x, y, heat_color(h));
    }
    fillrect(a, 16, SH - 2, SW - 17, SH - 1, RGB(60, 28, 14));                 /* logs */
    for (int x = 16; x < SW - 16; x += 4) put(a, x, SH - 2, RGB(120, 50, 16));
}

static void scene_thunder(App *a)
{
    int f = a->frame;
    if (a->bolt_timer > 0) a->bolt_timer--;
    if (a->bolt_timer <= 0 && !a->bolt_on) {
        a->bolt_on = TRUE; a->bolt_age = 0;
        int x = g_random_int_range(14, 50);
        for (int y = 0; y < SH; y++) {
            a->bolt_x[y] = x;
            if (y >= 7) { x += g_random_int_range(-2, 3); x = x < 4 ? 4 : x > SW - 5 ? SW - 5 : x; }
        }
    }
    double flash = 0;
    if (a->bolt_on) {
        a->bolt_age++;
        flash = a->bolt_age <= 2 ? 0.8 : a->bolt_age <= 5 ? 0.35 : a->bolt_age <= 9 ? 0.1 : 0;
        if (a->bolt_age > 10) { a->bolt_on = FALSE; a->bolt_timer = g_random_int_range(22, 60); }
    }
    gradient(a, 0, SH, mixc(RGB(8, 10, 34), RGB(255, 255, 255), flash * 0.5), mixc(RGB(30, 24, 70), RGB(255, 255, 255), flash * 0.6));
    for (int i = 0; i < 6; i++)
        cloud(a, pmod(i * 16 - f / 5, SW + 24) - 12, 5 + (i % 2) * 3, 20,
              mixc(RGB(50, 55, 88), RGB(235, 238, 255), flash), mixc(RGB(20, 22, 44), RGB(165, 172, 212), flash));
    if (a->bolt_on && a->bolt_age <= 8)
        for (int y = 8; y < SH - 5; y++) {
            line(a, a->bolt_x[y], y, a->bolt_x[y + 1], y + 1, RGB(255, 255, 255));
            blend(a, a->bolt_x[y] - 1, y, RGB(120, 220, 255), 0.45);
            blend(a, a->bolt_x[y] + 1, y, RGB(120, 220, 255), 0.45);
        }
    for (int i = 0; i < 44; i++) {                                              /* rain */
        int x0 = (int)(hash2(i, 1, 7) % SW), y = (int)((hash2(i, 2, 3) % SH) + (guint)(f * 3)) % SH;
        int xx = pmod(x0 - y / 3, SW);
        blend(a, xx, y, RGB(150, 180, 230), 0.7); blend(a, xx - 1, y + 1, RGB(120, 150, 210), 0.5);
    }
    for (int x = 0; x < SW; x++) fillrect(a, x, SH - 4 + (int)(2 * sin(x * 0.2)), x, SH - 1, RGB(7, 7, 15));
}

static void scene_sea(App *a)
{
    int f = a->frame, hz = 18;
    gradient(a, 0, hz, RGB(24, 18, 62), RGB(255, 125, 70));
    for (int i = 0; i < 14; i++) {
        int x = (int)(hash2(i, 3, 1) % SW), y = (int)(hash2(i, 4, 1) % 9);
        blend(a, x, y, RGB(255, 255, 255), 0.35 + 0.35 * sin(f * 0.2 + i));
    }
    disc(a, 32, hz - 2, 7, RGB(255, 205, 100), 1.0);
    for (int y = hz; y < SH; y++) {
        double t = (double)(y - hz) / (SH - hz);
        guint32 base = mixc(RGB(30, 80, 130), RGB(4, 18, 56), t);
        for (int x = 0; x < SW; x++) {
            double w = sin(x * 0.35 + f * 0.22 + y * 0.9) + 0.5 * sin(x * 0.15 - f * 0.13 + y * 1.7);
            guint32 c = base;
            if (w > 1.05) c = mixc(base, RGB(225, 242, 255), 0.7);
            else if (w > 0.6) c = mixc(base, RGB(90, 150, 200), 0.4);
            int dx = abs(x - 32);
            if (dx < 7 - (int)(t * 4)) c = mixc(c, RGB(255, 195, 105), (0.55 - dx * 0.07) * (0.5 + 0.5 * sin(f * 0.3 + y * 2 + x)));
            put(a, x, y, c);
        }
    }
    int bx = pmod(f / 3, SW + 20) - 10, by = hz + 7 + (int)(1.3 * sin(f * 0.15));
    fillrect(a, bx - 3, by, bx + 3, by, RGB(20, 12, 10)); fillrect(a, bx - 2, by + 1, bx + 2, by + 1, RGB(20, 12, 10));
    line(a, bx, by - 8, bx, by - 1, RGB(30, 20, 15));
    for (int i = 0; i < 6; i++) fillrect(a, bx + 1, by - 1 - i, bx + 1 + (6 - i) / 2, by - 1 - i, RGB(240, 232, 215));
}

static void scene_air(App *a)
{
    int f = a->frame;
    gradient(a, 0, SH, RGB(66, 146, 235), RGB(196, 228, 255));
    disc(a, 53, 7, 4, RGB(255, 244, 190), 0.9);
    for (int layer = 0; layer < 3; layer++)
        for (int j = 0; j < 3; j++)
            cloud(a, pmod(j * 30 + layer * 11 - f * (layer + 1) / 4, SW + 30) - 15, 6 + layer * 9 + j * 2, 14 + layer * 5,
                  RGB(255, 255, 255), RGB(204, 220, 246));
    for (int i = 0; i < 9; i++) {                                                /* wind streaks with curls */
        int y = 4 + i * 4 + (int)(hash2(i, 0, 2) % 3), len = 10 + (int)(hash2(i, 1, 2) % 8);
        int x0 = pmod(f * (2 + i % 3) + i * 13, SW + len + 10) - len;
        int yy = y;
        for (int k = 0; k < len; k++) {
            yy = y + (int)lround(1.3 * sin((x0 + k) * 0.3 + f * 0.1 + i));
            blend(a, x0 + k, yy, RGB(255, 255, 255), 0.8 - 0.5 * (1.0 - (double)k / len));
        }
        blend(a, x0 + len, yy - 1, RGB(255, 255, 255), 0.7); blend(a, x0 + len + 1, yy - 2, RGB(255, 255, 255), 0.6);
        blend(a, x0 + len + 2, yy - 2, RGB(255, 255, 255), 0.5);
    }
    for (int i = 0; i < 14; i++) {                                               /* drifting leaves/dust */
        int x = (i * 5 + f) % SW, y = 28 + (int)(7 * sin(f * 0.08 + i * 0.45 + x * 0.1));
        blend(a, x, y, RGB(255, 255, 255), 0.55);
    }
    for (int x = 0; x < SW; x++) fillrect(a, x, SH - 3 + (int)(1.5 * sin(x * 0.25 + 1)), x, SH - 1, RGB(58, 138, 80));
}

static void scene_dark(App *a)
{
    int f = a->frame;
    gradient(a, 0, SH, RGB(10, 8, 22), RGB(70, 34, 52));
    for (int y = SH - 9; y < SH; y++)                                  /* sullen glow under the storm */
        for (int x = 0; x < SW; x++)
            blend(a, x, y, RGB(230, 90, 40), (0.10 + 0.20 * (double)(y - (SH - 9)) / 9) * (0.8 + 0.2 * sin(f * 0.1 + x * 0.2)));
    double pulse = (f % 110) < 7 ? 0.55 * (1.0 - (f % 110) / 7.0) : 0;  /* silent flicker inside the clouds */
    for (int L = 0; L < 3; L++)
        for (int j = 0; j < 3; j++) {
            int w = 20 + L * 6 + (int)(hash2(j, L, 4) % 8);
            cloud(a, pmod(j * 36 + L * 15 - f * (L + 1) / 3, SW + 44) - 22, 7 + L * 10 + (int)(hash2(j, L, 6) % 5), w,
                  mixc(RGB(78 + L * 20, 68 + L * 16, 110 + L * 16), RGB(190, 150, 235), pulse),
                  RGB(18 + L * 5, 14 + L * 4, 32 + L * 5));
        }
    for (int i = 0; i < 18; i++) {                                     /* distant drizzle */
        int x = (int)(hash2(i, 1, 2) % SW), y = (int)((hash2(i, 2, 2) % 14) + (guint)(f * 2)) % 14 + 24;
        blend(a, x, y, RGB(150, 140, 200), 0.35);
    }
}

static void scene_horus(App *a)
{
    int f = a->frame, cx = 30, cy = 19, A = 22;
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) {
            double d = hypot(x - cx, y - cy) / 30.0, g = d < 1 ? (1 - d) * (0.55 + 0.45 * sin(f * 0.1)) : 0;
            guint32 c = mixc(RGB(6, 10, 28), RGB(40, 30, 10), g);
            if (x % 6 == 0 && y % 6 == 0) c = mixc(c, RGB(230, 180, 60), 0.15);
            put(a, x, y, c);
        }
    int bp = f % 80; double open = 1;
    if (bp >= 72) { int k = bp - 72; open = k < 4 ? 1 - k / 3.0 : (k - 3) / 4.0; }       /* blink */
    for (int x = cx - A; x <= cx + A; x++) {
        double t = (x - cx) / (double)A, k = 1 - t * t;
        int yu = cy - (int)lround(9 * k), yl = cy + (int)lround(5 * k);
        double sh = 0.75 + 0.25 * sin((x - f * 1.2) * 0.35);
        guint32 gold = mixc(RGB(176, 126, 20), RGB(255, 228, 130), sh);
        for (int y = yu; y <= yl; y++) {
            guint32 c = RGB(5, 8, 20);
            int dx = x - (cx + 1), dy = y - (cy - 1), r2 = dx * dx + dy * dy;
            if (r2 <= 36 && abs(dy) <= 6 * open + 0.5) {
                double rr = sqrt((double)r2);
                c = mixc(RGB(90, 225, 255), RGB(10, 60, 170), rr / 6.0);
                c = mixc(c, RGB(255, 255, 255), 0.12 * sin(f * 0.2) + 0.1);
                if (r2 <= 6) c = RGB(2, 4, 12);
                if (dx == -2 && dy == -2) c = RGB(255, 255, 255);
            }
            if (y < cy - open * 8 || y > cy + open * 5) c = RGB(100, 72, 16);           /* eyelid */
            put(a, x, y, c);
        }
        put(a, x, yu, gold); put(a, x, yu - 1, gold); put(a, x, yl, gold); put(a, x, yl + 1, gold);
    }
    line(a, cx + A, cy, cx + A + 9, cy + 3, RGB(240, 200, 80)); line(a, cx + A, cy + 1, cx + A + 9, cy + 4, RGB(200, 150, 40));
    put(a, cx - A - 1, cy, RGB(240, 200, 80)); put(a, cx - A - 2, cy, RGB(200, 150, 40));
    for (int x = cx - 22; x <= cx + 24; x++) {                                          /* brow */
        int yb = cy - 14 - (int)lround(3 * sin(M_PI * (x - (cx - 22)) / 46.0));
        put(a, x, yb, RGB(40, 90, 210)); put(a, x, yb - 1, RGB(90, 150, 255));
    }
    for (int y = cy + 6; y <= cy + 16; y++) { put(a, cx - 5, y, RGB(240, 200, 80)); put(a, cx - 6, y, RGB(176, 126, 20)); }
    for (double th = M_PI; th < M_PI * 4.2; th += 0.1) {                               /* curl */
        double r = 7 * (1 - (th - M_PI) / (3.2 * M_PI));
        put(a, (int)lround(cx + 3 + r * cos(th)), (int)lround(cy + 16 - r * sin(th) * 0.7), RGB(240, 200, 80));
    }
}

static void scene_cross(App *a)
{
    int f = a->frame;
    for (int y = 0; y < SH; y++) {
        double t = (double)y / (SH - 1);
        guint32 c = t < 0.6 ? mixc(RGB(12, 14, 48), RGB(120, 60, 110), t / 0.6) : mixc(RGB(120, 60, 110), RGB(255, 150, 80), (t - 0.6) / 0.4);
        for (int x = 0; x < SW; x++) put(a, x, y, c);
    }
    for (int i = 0; i < 30; i++) {
        int x = (int)(hash2(i, 1, 5) % SW), y = (int)(hash2(i, 2, 5) % 16);
        blend(a, x, y, RGB(255, 255, 255), 0.3 + 0.6 * (0.5 + 0.5 * sin(f * 0.15 + i)));
    }
    for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) {
        double d = hypot(x - 32, (y - 8) * 1.1);
        if (d < 22) blend(a, x, y, RGB(255, 205, 125), (1 - d / 22) * 0.38 * (0.8 + 0.2 * sin(f * 0.12)));
    }
    for (int i = 0; i < 14; i++) {                                                      /* rays */
        double ang = i * (2 * M_PI / 14) + f * 0.02;
        for (int r = 3; r < 20; r++)
            blend(a, (int)lround(32 + cos(ang) * r), (int)lround(8 + sin(ang) * r * 0.9), RGB(255, 232, 165),
                  (1 - r / 20.0) * (i % 2 ? 0.28 : 0.16));
    }
    for (int x = 0; x < SW; x++) {                                                      /* far range */
        int h = SH - 14 - (int)(5 * sin(x * 0.13 + 1) + 3 * sin(x * 0.31));
        for (int y = h; y < SH; y++) put(a, x, y, mixc(RGB(80, 56, 112), RGB(40, 28, 70), (double)(y - h) / 14));
    }
    for (int x = 4; x < SW - 4; x++) {                                                  /* main mountain */
        int dx = abs(x - 32), h = 14 + (int)(dx * 0.85) + (int)(hash2(x, 0, 9) % 3);
        for (int y = h; y < SH; y++) {
            guint32 c = mixc(RGB(20, 22, 50), RGB(8, 10, 24), (double)(y - h) / 24);
            if (y < h + 4) c = x < 32 ? RGB(236, 232, 248) : RGB(170, 168, 205);
            else if (x < 32 && y < h + 9) c = mixc(c, RGB(110, 84, 140), 0.5);
            put(a, x, y, c);
        }
    }
    guint32 cc = mixc(RGB(255, 236, 170), RGB(255, 255, 255), 0.5 + 0.5 * sin(f * 0.2));
    fillrect(a, 31, 3, 32, 13, cc); fillrect(a, 28, 6, 35, 7, cc);                        /* the cross */
    for (int i = 0; i < 2; i++)
        cloud(a, pmod(i * 40 - f / 3, SW + 40) - 20, SH - 11 + i * 2, 20, RGB(236, 176, 176), RGB(150, 100, 130));
    fillrect(a, 0, SH - 2, SW - 1, SH - 1, RGB(5, 7, 14));
}

static void pyramid(App *a, int cx, int base, int half, int h)
{
    for (int y = base - h; y <= base; y++) {
        double t = (double)(y - (base - h)) / h, w = t * half;
        for (int x = (int)(cx - w); x <= (int)(cx + w); x++) {
            guint32 c = x < cx ? mixc(RGB(238, 192, 112), RGB(150, 100, 60), t * 0.5)
                               : mixc(RGB(122, 82, 72), RGB(60, 40, 50), t * 0.5);
            if (y % 3 == 0) c = mixc(c, RGB(40, 25, 25), 0.18);
            if (x == cx) c = RGB(255, 226, 160);
            put(a, x, y, c);
        }
    }
}

static void scene_pyramid(App *a)
{
    int f = a->frame;
    gradient(a, 0, SH, RGB(8, 10, 36), RGB(214, 112, 72));
    for (int i = 0; i < 26; i++) {
        int x = (int)(hash2(i, 1, 8) % SW), y = (int)(hash2(i, 2, 8) % 18);
        blend(a, x, y, RGB(255, 255, 255), 0.25 + 0.6 * (0.5 + 0.5 * sin(f * 0.13 + i * 1.7)));
    }
    disc(a, 51, 8, 6, RGB(255, 245, 200), 0.18); disc(a, 51, 8, 4, RGB(250, 246, 220), 1.0);
    blend(a, 50, 7, RGB(200, 200, 190), 0.6); blend(a, 52, 9, RGB(200, 200, 190), 0.6);
    int k = f % 90;
    if (k < 10) { for (int tl = 0; tl < 5; tl++) blend(a, 10 + k * 4 - tl * 2, 2 + k * 2 - tl, RGB(255, 255, 255), 0.8 - tl * 0.15); }
    pyramid(a, 12, 31, 12, 13); pyramid(a, 56, 31, 10, 11); pyramid(a, 36, 31, 22, 24);
    for (int x = 0; x < SW; x++) {
        int gy = 31 + (int)lround(1.5 * sin(x * 0.25));
        for (int y = gy; y < SH; y++) {
            guint32 c = mixc(RGB(196, 144, 82), RGB(86, 58, 40), (double)(y - gy) / 9);
            if (hash2(x, y, f / 2) % 37 == 0) c = RGB(255, 226, 160);
            put(a, x, y, c);
        }
    }
}

static void scene_dancer(App *a)                    /* a generic stage silhouette; not any real person */
{
    int f = a->frame;
    gradient(a, 0, SH, RGB(10, 6, 22), RGB(26, 18, 42));
    for (int i = 0; i < 6; i++)
        if (hash2(i, f / 3, 4) % 9 == 0)
            blend(a, (int)(hash2(i, 5, 4) % SW), (int)(hash2(i, 6, 4) % 10), RGB(255, 255, 255), 0.9);
    for (int y = 0; y < 34; y++) {                                                      /* spotlight */
        double half = 2 + y * 0.55;
        for (int x = (int)(32 - half); x <= (int)(32 + half); x++) {
            double edge = 1 - abs(x - 32) / (half + 1);
            blend(a, x, y, RGB(255, 245, 200), 0.24 * edge * (1 - y / 60.0));
        }
    }
    for (int dy = -3; dy <= 3; dy++) for (int dx = -18; dx <= 18; dx++)
        if ((dx * dx) / 324.0 + (dy * dy) / 9.0 <= 1) blend(a, 32 + dx, 35 + dy, RGB(255, 235, 180), 0.35);
    int x = 32 + (int)(12 * sin(f * 0.06)), o1 = (int)lround(2 * sin(f * 0.3)), o2 = -o1;
    guint32 sil = RGB(4, 3, 8), rim = RGB(130, 110, 170);
    line(a, x - 1, 25, x - 1 + o1, 33, sil); line(a, x, 25, x + o1, 33, sil);            /* legs, gliding */
    line(a, x + 1, 25, x + 1 + o2, 33, sil); line(a, x + 2, 25, x + 2 + o2, 33, sil);
    fillrect(a, x - 1 + o1, 34, x + 1 + o1, 34, sil); fillrect(a, x + 1 + o2, 34, x + 3 + o2, 34, sil);
    fillrect(a, x - 2, 16, x + 3, 24, sil);                                              /* jacket */
    line(a, x + 3, 17, x + 7, 11, sil); line(a, x - 2, 17, x - 4, 23, sil);              /* arms */
    fillrect(a, x - 1, 12, x + 2, 15, sil);                                              /* head */
    fillrect(a, x - 3, 11, x + 4, 11, sil); fillrect(a, x - 1, 8, x + 2, 10, sil);       /* hat */
    for (int y = 16; y <= 24; y++) put(a, x - 2, y, rim);
    put(a, x - 1, 12, rim);
}

static void glyph(App *a, const char *const rows[5], int x, int y, int s, guint32 c)
{
    for (int r = 0; r < 5; r++) for (int q = 0; q < 3; q++)
        if (rows[r][q] == '1') fillrect(a, x + q * s, y + r * s, x + q * s + s - 1, y + r * s + s - 1, c);
}

static void scene_relativity(App *a)                /* chalkboard + warped spacetime grid + E=MC2 */
{
    static const char *const GLY_E[5] = { "111", "100", "110", "100", "111" }, *const GLY_EQ[5] = { "000", "111", "000", "111", "000" },
                             *const GLY_M[5] = { "101", "111", "111", "101", "101" }, *const GLY_C[5] = { "111", "100", "100", "100", "111" },
                             *const GLY_2[5] = { "111", "001", "111", "100", "111" };
    int f = a->frame, lf = f % 150;
    gradient(a, 0, SH, RGB(8, 28, 24), RGB(12, 40, 33));
    for (int x = 0; x < SW; x++) { put(a, x, 0, RGB(110, 72, 34)); put(a, x, SH - 1, RGB(110, 72, 34)); }
    for (int y = 0; y < SH; y++) { put(a, 0, y, RGB(110, 72, 34)); put(a, SW - 1, y, RGB(110, 72, 34)); }
    guint32 grid = RGB(70, 160, 136);
    for (int gy = 24; gy <= 38; gy += 4)
        for (int x = 1; x < SW - 1; x++)
            put(a, x, gy + (int)lround(7 * exp(-((x - 32.0) * (x - 32.0)) / 90.0) * (gy - 20) / 18.0), grid);
    for (int gx = 4; gx < SW - 3; gx += 6)
        for (int y = 22; y < SH - 1; y++)
            put(a, (int)lround(gx + (32 - gx) * 0.3 * exp(-((y - 34.0) * (y - 34.0)) / 30.0)), y, mixc(grid, RGB(20, 60, 50), 0.4));
    disc(a, 32, 34, 3, RGB(255, 214, 90), 0.6 + 0.4 * sin(f * 0.2));
    for (int tr = 0; tr < 6; tr++) {                                                      /* orbiting probe + trail */
        double ang = f * 0.09 - tr * 0.18;
        blend(a, (int)lround(32 + 20 * cos(ang)), (int)lround(33 + 5 * sin(ang)), RGB(120, 230, 255), 0.9 - tr * 0.15);
    }
    int n = lf / 12; if (n > 5) n = 5;
    guint32 chalk = lf > 60 ? mixc(RGB(235, 240, 225), RGB(255, 235, 140), 0.5 + 0.5 * sin(f * 0.3)) : RGB(235, 240, 225);
    if (n >= 1) glyph(a, GLY_E, 6, 3, 3, chalk);
    if (n >= 2) glyph(a, GLY_EQ, 18, 3, 3, chalk);
    if (n >= 3) glyph(a, GLY_M, 30, 3, 3, chalk);
    if (n >= 4) glyph(a, GLY_C, 42, 3, 3, chalk);
    if (n >= 5) glyph(a, GLY_2, 54, 1, 2, chalk);
    for (int i = 0; i < 8; i++) blend(a, (int)(hash2(i, f / 2, 3) % 60) + 2, (int)(hash2(i, f / 2, 6) % 18) + 2, RGB(255, 255, 255), 0.3);
}

static void scene_custom(App *a)                    /* ~/.hterm/custom_scene.txt : your own 64x40 sprite */
{
    static const guint32 PAL[16] = { 0x000000, 0x1b2438, 0xff5c7a, 0x00e5a0, 0xffb454, 0x3aa0ff, 0xc792ea, 0xc8e8ff,
                                     0x5b7fa0, 0xff8099, 0x4dffc3, 0xffd080, 0x7ab8ff, 0xdcb0ff, 0x5fd8f0, 0xffffff };
    int f = a->frame;
    gradient(a, 0, SH, RGB(4, 6, 14), RGB(12, 16, 34));
    for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) {
        int v = a->custom[y * SW + x];
        if (!v) continue;
        guint32 c = PAL[v - 1];
        c = mixc(c, RGB(255, 255, 255), (y == f % SH ? 0.45 : 0.0) + 0.06 * sin(f * 0.2 + x * 0.3));
        put(a, x, y, c);
    }
}

static void scene_render(App *a)
{
    switch (a->scene) {
    case SC_FIRE: scene_fire(a); break;           case SC_THUNDER: scene_thunder(a); break;
    case SC_SEA: scene_sea(a); break;             case SC_AIR: scene_air(a); break;
    case SC_DARK: scene_dark(a); break;           case SC_HORUS: scene_horus(a); break;
    case SC_CROSS: scene_cross(a); break;         case SC_PYRAMID: scene_pyramid(a); break;
    case SC_DANCER: scene_dancer(a); break;       case SC_RELATIVITY: scene_relativity(a); break;
    default: scene_custom(a); break;
    }
}

static void scene_set(App *a, int idx)
{
    a->scene = pmod(idx, a->nscenes);
    a->frame = 0; a->trans = 8; a->bolt_on = FALSE; a->bolt_timer = 6;
    memset(a->heat, 0, sizeof a->heat);
    char *t = g_strdup_printf("◈ %s   · click: next · right-click: previous", SCENE_NAMES[a->scene]);
    gtk_label_set_text(GTK_LABEL(a->scene_lbl), t);
    g_free(t);
    scene_render(a);
}

static gboolean scene_tick(gpointer d)
{
    App *a = d;
    a->frame++;
    if (a->frame >= SCENE_FRAMES) scene_set(a, a->scene + 1);
    if (a->trans > 0) a->trans--;
    scene_render(a);
    gtk_widget_queue_draw(a->scene_area);
    return G_SOURCE_CONTINUE;
}

static gboolean draw_scene(GtkWidget *w, cairo_t *cr, gpointer d)
{
    App *a = d;
    double cell = gtk_widget_get_allocated_width(w) / (double)SW;
    for (int y = 0; y < SH; y++) {
        int x = 0;
        while (x < SW) {                                                                  /* merge equal-colour runs */
            guint32 c = a->px[y * SW + x];
            int e = x + 1;
            while (e < SW && a->px[y * SW + e] == c) e++;
            cairo_set_source_rgb(cr, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0);
            cairo_rectangle(cr, x * cell, y * cell, (e - x) * cell + 0.5, cell + 0.5);
            cairo_fill(cr);
            x = e;
        }
    }
    cairo_set_source_rgba(cr, 0, 0, 0, 0.16);                                             /* scanlines */
    for (int y = 0; y < SH; y++) { cairo_rectangle(cr, 0, y * cell + cell - 1, SW * cell, 1); cairo_fill(cr); }
    if (a->trans > 0) {                                                                   /* switch glitch */
        cairo_set_source_rgba(cr, 0.02, 0.05, 0.08, 0.95);
        for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++)
            if ((int)(hash2(x, y, a->frame) % 8) < a->trans) { cairo_rectangle(cr, x * cell, y * cell, cell, cell); cairo_fill(cr); }
    }
    return FALSE;
}

static gboolean on_scene_click(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
    (void)w;
    App *a = d;
    if (ev->type != GDK_BUTTON_PRESS) return TRUE;
    scene_set(a, a->scene + (ev->button == 3 ? -1 : 1));
    gtk_widget_queue_draw(a->scene_area);
    return TRUE;
}

static void load_custom_scene(App *a)
{
    char *path = g_build_filename(a->home, HIST_DIR_NAME, "custom_scene.txt", NULL);
    gchar *txt = NULL;
    if (g_file_get_contents(path, &txt, NULL, NULL)) {
        gchar **ln = g_strsplit(txt, "\n", -1);
        for (int y = 0; y < SH && ln[y]; y++)
            for (int x = 0; x < SW && ln[y][x] && ln[y][x] != '\r'; x++) {
                int ch = g_ascii_xdigit_value(ln[y][x]);
                a->custom[y * SW + x] = ch >= 0 ? (guint8)(ch + 1) : 0;
            }
        g_strfreev(ln);
        a->has_custom = TRUE;
        g_free(txt);
    }
    g_free(path);
    a->nscenes = a->has_custom ? SC_COUNT : SC_COUNT - 1;
}

/* ===================================================================== */
/*  Dashboard                                                             */
/* ===================================================================== */

static gboolean draw_graph(GtkWidget *w, cairo_t *cr, gpointer d)
{
    App *a = d;
    int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    double bw = (double)W / GRAPH_N;
    for (int i = 0; i < GRAPH_N; i++) {
        double v = a->cpu_hist[(a->cpu_pos + i) % GRAPH_N], h = v / 100.0 * H;
        if (h < 1) h = 1;
        cairo_set_source_rgba(cr, 0, .9, .63, .4 + .6 * v / 100.0);
        cairo_rectangle(cr, i * bw, H - h, bw - 1, h);
        cairo_fill(cr);
    }
    return FALSE;
}

static GtkWidget *panel_new(const char *title, GtkWidget **right)
{
    GtkWidget *p = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *t = gtk_label_new(title);
    gtk_style_context_add_class(gtk_widget_get_style_context(p), "panel");
    gtk_style_context_add_class(gtk_widget_get_style_context(t), "title");
    gtk_box_pack_start(GTK_BOX(hdr), t, FALSE, FALSE, 0);
    *right = gtk_label_new("--");
    gtk_style_context_add_class(gtk_widget_get_style_context(*right), "pct");
    gtk_box_pack_end(GTK_BOX(hdr), *right, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p), hdr, FALSE, FALSE, 0);
    return p;
}

static GtkWidget *bar_new(void)
{
    GtkWidget *b = gtk_progress_bar_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "thin");
    return b;
}

static GtkWidget *build_dashboard(App *a)
{
    GtkWidget *dash = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *p;

    /* ---- SYSTEM MONITOR: CPU | MEMORY side by side ---- */
    GtkWidget *mon = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    p = panel_new("CPU", &a->cpu_lbl);
    a->cpu_graph = gtk_drawing_area_new();
    gtk_widget_set_size_request(a->cpu_graph, -1, 44);
    g_signal_connect(a->cpu_graph, "draw", G_CALLBACK(draw_graph), a);
    gtk_box_pack_start(GTK_BOX(p), a->cpu_graph, FALSE, FALSE, 0);
    GtkWidget *cores = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(cores), 6);
    for (int i = 0; i < 4; i++) {
        char n[16]; g_snprintf(n, sizeof n, "CPU%d", i);
        a->core_bar[i] = bar_new();
        gtk_widget_set_size_request(a->core_bar[i], 44, -1);
        gtk_widget_set_valign(a->core_bar[i], GTK_ALIGN_CENTER);
        a->core_lbl[i] = label_new("0%", "pct");
        int col = (i % 2) * 3, row = i / 2;
        gtk_grid_attach(GTK_GRID(cores), label_new(n, "dim"), col, row, 1, 1);
        gtk_grid_attach(GTK_GRID(cores), a->core_bar[i], col + 1, row, 1, 1);
        gtk_grid_attach(GTK_GRID(cores), a->core_lbl[i], col + 2, row, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(p), cores, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mon), p, TRUE, TRUE, 0);

    p = panel_new("MEMORY", &a->mem_lbl);
    a->mem_bar = bar_new(); a->mem_txt = label_new("", "pct");
    gtk_box_pack_start(GTK_BOX(p), a->mem_bar, FALSE, FALSE, 2);
    gtk_widget_set_halign(a->mem_txt, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(p), a->mem_txt, FALSE, FALSE, 0);
    GtkWidget *swhdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    a->swap_lbl = label_new("0%", "pct");
    gtk_box_pack_start(GTK_BOX(swhdr), label_new("SWAP", "dim"), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(swhdr), a->swap_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p), swhdr, FALSE, FALSE, 0);
    a->swap_bar = bar_new(); a->swap_txt = label_new("", "pct");
    gtk_widget_set_halign(a->swap_txt, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(p), a->swap_bar, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(p), a->swap_txt, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mon), p, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(dash), mon, FALSE, FALSE, 0);

    /* ---- TASKS ---- */
    p = panel_new("RUNNING TASKS", &a->task_lbl);
    a->store = gtk_list_store_new(6, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(a->store));
    const char *cols[] = { "PID", "NAME", "CPU%", "MEM%", "STATUS" };
    for (int i = 0; i < 5; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        if (i == 2 || i == 3) g_object_set(r, "xalign", 1.0, NULL);
        GtkTreeViewColumn *c = (i == 4)
            ? gtk_tree_view_column_new_with_attributes(cols[i], r, "text", i, "foreground", 5, NULL)
            : gtk_tree_view_column_new_with_attributes(cols[i], r, "text", i, NULL);
        if (i == 1) gtk_tree_view_column_set_expand(c, TRUE);
        if (i == 2 || i == 3) gtk_tree_view_column_set_alignment(c, 1.0);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c);
    }
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), GTK_SELECTION_NONE);
    gtk_box_pack_start(GTK_BOX(p), tree, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(dash), p, FALSE, FALSE, 0);

    /* ---- FILE EXPLORER ---- */
    p = panel_new("FILE EXPLORER", &a->ex_path);
    gtk_label_set_ellipsize(GTK_LABEL(a->ex_path), PANGO_ELLIPSIZE_START);
    gtk_label_set_max_width_chars(GTK_LABEL(a->ex_path), 48);
    GtkWidget *cols2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_box_pack_start(GTK_BOX(left), label_new("Current Folder", "dim"), FALSE, FALSE, 0);
    GtkWidget *curhdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(curhdr), "curdir");
    a->cur_name = label_new("", NULL); a->cur_cnt = label_new("", "dim");
    gtk_box_pack_start(GTK_BOX(curhdr), a->cur_name, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(curhdr), a->cur_cnt, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), curhdr, FALSE, FALSE, 0);
    a->cur_store = gtk_tree_store_new(C_N, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
    gtk_box_pack_start(GTK_BOX(left), tree_new(GTK_TREE_MODEL(a->cur_store), &a->cur_tv, a), TRUE, TRUE, 0);
    g_signal_connect(a->cur_tv, "test-expand-row", G_CALLBACK(on_test_expand), a);
    gtk_box_pack_start(GTK_BOX(cols2), left, TRUE, TRUE, 0);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_box_pack_start(GTK_BOX(right), label_new("Previous Folder", "dim"), FALSE, FALSE, 0);
    GtkWidget *parhdr = gtk_event_box_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(parhdr), "parhdr");
    a->par_lbl = label_new("", "pct");
    gtk_label_set_ellipsize(GTK_LABEL(a->par_lbl), PANGO_ELLIPSIZE_START);
    gtk_container_add(GTK_CONTAINER(parhdr), a->par_lbl);
    g_signal_connect(parhdr, "button-press-event", G_CALLBACK(on_parent_click), a);
    gtk_box_pack_start(GTK_BOX(right), parhdr, FALSE, FALSE, 0);
    a->par_store = gtk_list_store_new(C_N, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
    gtk_box_pack_start(GTK_BOX(right), tree_new(GTK_TREE_MODEL(a->par_store), &a->par_tv, a), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(cols2), right, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(p), cols2, FALSE, FALSE, 0);
    a->ex_help = label_new("Click a folder to cd into it · Alt+↑ goes to the parent", "dim");
    gtk_box_pack_start(GTK_BOX(p), a->ex_help, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(dash), p, FALSE, FALSE, 0);

    /* ---- PROFILE (flame instead of a portrait) ---- */
    p = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_style_context_add_class(gtk_widget_get_style_context(p), "panel");
    gtk_style_context_add_class(gtk_widget_get_style_context(p), "profile");
    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(frame), "flameframe");
    a->scene_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(a->scene_area, SW * 3, SH * 3);
    gtk_widget_add_events(a->scene_area, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(a->scene_area, "draw", G_CALLBACK(draw_scene), a);
    g_signal_connect(a->scene_area, "button-press-event", G_CALLBACK(on_scene_click), a);
    gtk_box_pack_start(GTK_BOX(frame), a->scene_area, FALSE, FALSE, 0);
    gtk_widget_set_valign(frame, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(p), frame, FALSE, FALSE, 0);
    GtkWidget *txt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(txt), label_new("Hubert Spencer", "name"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(txt), label_new("Developer  |  Problem Solver  |  Always Learning", "role"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(txt), label_new("“Better Code. Better Tomorrow.”", "pct"), FALSE, FALSE, 6);
    a->scene_lbl = label_new("", "dim");
    gtk_label_set_ellipsize(GTK_LABEL(a->scene_lbl), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(txt), a->scene_lbl, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(p), txt, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(dash), p, FALSE, FALSE, 0);

    /* ---- QUICK COMMANDS ---- */
    GtkWidget *qh;
    p = panel_new("QUICK COMMANDS", &qh);
    gtk_label_set_text(GTK_LABEL(qh), "click: insert · double-click: run · right-click: copy");
    GtkWidget *grid = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);

    GtkWidget *c0 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(c0), cmd_group(a, "C & BUILD", "#00ff9c"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c0), cmd_group(a, "FILE EDITING", "#5fd8f0"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(grid), c0, TRUE, TRUE, 0);

    GtkWidget *c1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(c1), cmd_group(a, "NAVIGATION", "#3aa0ff"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c1), cmd_group(a, "VI KEYS", "#ff8099"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c1), cmd_group(a, "SYSTEM UTILITIES", "#ffb454"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(grid), c1, TRUE, TRUE, 0);

    GtkWidget *c2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(c2), cmd_group(a, "GIT COMMANDS", "#c792ea"), FALSE, FALSE, 0);
    GtkWidget *gi = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_box_pack_start(GTK_BOX(gi), label_new("GIT INFO", "title"), FALSE, FALSE, 2);
    a->gi_repo = ellipsized(NULL);   a->gi_branch = ellipsized(NULL); a->gi_remote = ellipsized(NULL);
    a->gi_url = ellipsized(NULL);    a->gi_status = ellipsized(NULL); a->gi_last = ellipsized(NULL);
    GtkWidget *rows[] = { a->gi_repo, a->gi_branch, a->gi_remote, a->gi_url, a->gi_status, a->gi_last };
    for (int i = 0; i < 6; i++) gtk_box_pack_start(GTK_BOX(gi), rows[i], FALSE, FALSE, 0);
    a->gi_open = gtk_button_new_with_label("Open remote ↗");
    gtk_style_context_add_class(gtk_widget_get_style_context(a->gi_open), "openbtn");
    gtk_widget_set_halign(a->gi_open, GTK_ALIGN_START);
    g_signal_connect(a->gi_open, "clicked", G_CALLBACK(on_open_remote), a);
    gtk_box_pack_start(GTK_BOX(gi), a->gi_open, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(c2), gi, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(grid), c2, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(p), grid, FALSE, FALSE, 0);
    a->cmd_help = label_new("hover a command to see what it does", "dim");
    gtk_label_set_ellipsize(GTK_LABEL(a->cmd_help), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(p), a->cmd_help, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(dash), p, FALSE, FALSE, 0);

    /* whole right side scrolls when the window is short */
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), dash);
    return sw;
}

static const char *status_text(char s, const char **color)
{
    switch (s) {
    case 'R': *color = "#00ff9c"; return "Running";
    case 'T': *color = "#ff5c7a"; return "Stopped";
    case 'Z': *color = "#ffb454"; return "Zombie";
    default:  *color = "#5aa9ff"; return "Sleeping";
    }
}

static void update_dashboard(App *a)
{
    SysInfo *s = a->si;
    sysinfo_update(s);
    char b[64];

    g_snprintf(b, sizeof b, "%.0f%%", s->cpu_total);
    gtk_label_set_text(GTK_LABEL(a->cpu_lbl), b);
    a->cpu_hist[a->cpu_pos] = s->cpu_total;
    a->cpu_pos = (a->cpu_pos + 1) % GRAPH_N;
    gtk_widget_queue_draw(a->cpu_graph);
    for (int i = 0; i < 4; i++) {
        double v = i < s->ncores ? s->cpu_core[i] : 0;
        g_snprintf(b, sizeof b, "%.0f%%", v);
        gtk_label_set_text(GTK_LABEL(a->core_lbl[i]), b);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->core_bar[i]), v / 100.0);
    }
    g_snprintf(b, sizeof b, "%.0f%%", s->mem_pct);
    gtk_label_set_text(GTK_LABEL(a->mem_lbl), b);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->mem_bar), s->mem_pct / 100.0);
    g_snprintf(b, sizeof b, "%.1fG / %.1fG", s->mem_used_gb, s->mem_total_gb);
    gtk_label_set_text(GTK_LABEL(a->mem_txt), b);
    g_snprintf(b, sizeof b, "%.0f%%", s->swap_pct);
    gtk_label_set_text(GTK_LABEL(a->swap_lbl), b);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->swap_bar), s->swap_pct / 100.0);
    g_snprintf(b, sizeof b, "%.1fG / %.1fG", s->swap_used_gb, s->swap_total_gb);
    gtk_label_set_text(GTK_LABEL(a->swap_txt), b);

    g_snprintf(b, sizeof b, "%d/%d", s->nrunning, s->nprocs);
    gtk_label_set_text(GTK_LABEL(a->task_lbl), b);
    gtk_list_store_clear(a->store);
    for (int i = 0; i < TASK_ROWS && i < s->nprocs; i++) {
        char pid[16], cpu[16], mem[16]; GtkTreeIter it; const char *col;
        g_snprintf(pid, sizeof pid, "%d", s->procs[i].pid);
        g_snprintf(cpu, sizeof cpu, "%.1f", s->procs[i].cpu);
        g_snprintf(mem, sizeof mem, "%.1f", s->procs[i].mem);
        const char *st = status_text(s->procs[i].state, &col);
        gtk_list_store_append(a->store, &it);
        gtk_list_store_set(a->store, &it, 0, pid, 1, s->procs[i].name, 2, cpu, 3, mem, 4, st, 5, col, -1);
    }
}

static gboolean on_tick(gpointer d)
{
    App *a = d;
    update_dashboard(a);
    update_status(a);
    return G_SOURCE_CONTINUE;
}

/* ===================================================================== */
/*  Window / theme                                                        */
/* ===================================================================== */

static const char *CSS =
    "window { background:#050a18; }"
    ".topbar { background:#07101f; border-bottom:1px solid #0e3a3a; padding:5px 12px; }"
    ".tab { color:#00ff9c; font-family:monospace; }"
    "textview, textview text { background-color:#050a18; color:#d8f3ff;"
    "  font-family:'JetBrains Mono','Fira Code','DejaVu Sans Mono',monospace; font-size:13px; }"
    "scrolledwindow { border:none; }"
    ".panel { background:#071425; border:1px solid #0f5a55; border-radius:0; padding:8px; margin:6px 8px 0 4px; }"
    ".profile { border-color:#00e5a0; }"
    ".title { color:#3aa0ff; font-weight:bold; font-family:monospace; }"
    ".name { color:#00ff9c; font-size:20px; font-weight:bold; font-family:monospace; }"
    ".pct { color:#00e5a0; font-weight:bold; font-family:monospace; }"
    ".dim { color:#5b7fa0; font-family:monospace; }"
    "progressbar.thin trough { min-height:6px; background:#0a1a2d; border:none; border-radius:0; }"
    "progressbar.thin progress { min-height:6px; border:none; border-radius:0;"
    "  background-image:linear-gradient(to right,#00e5a0,#17b8d8); }"
    "treeview { background-color:#071425; color:#c8e8ff; font-family:monospace; }"
    "treeview header button { background:#071425; color:#4a78a8; border:none; font-family:monospace; }"
    ".status { background:#07101f; color:#9fd0e0; padding:4px 12px; border-top:1px solid #0e3a3a;"
    "  font-family:monospace; }"
    ".role { color:#c8e8ff; font-family:monospace; }"
    ".flameframe { border:2px solid #00e5a0; background:#12060a; }"
    ".curdir { background:#0b2b46; color:#d8f3ff; padding:3px 6px; font-family:monospace; }"
    ".curdir label { color:#d8f3ff; }"
    ".parhdr { padding:3px 4px; }"
    ".parhdr:hover { background:#0b2b46; }"
    ".cmd { padding:1px 4px; font-family:monospace; font-size:11px; }"
    ".cmd:hover { background:#0d2a44; }"
    ".openbtn { background:#0b2b46; color:#00ff9c; border:1px solid #0f5a55; border-radius:0;"
    "  padding:1px 8px; font-family:monospace; font-size:11px; }"
    "treeview:selected { background-color:#0b2b46; color:#ffffff; }"
    ".edhdr { background:#07101f; border-bottom:1px solid #0e3a3a; padding:4px 12px; }"
    ".pane { border:1px solid #0b1b2b; }"
    ".pane-active { border:1px solid #0f5a55; }"
    ".tbtn { background:#0b2b46; color:#00ff9c; border:1px solid #0f5a55; border-radius:0;"
    "  padding:0 8px; margin-left:6px; font-family:monospace; font-size:11px; min-height:20px; }"
    ".tbtn:hover { background:#0f3d63; }"
    "paned > separator { background:#0e3a3a; min-width:1px; min-height:1px; }";


static void measure_font(App *a)
{
    a->font = pango_font_description_from_string("Menlo,Monaco,DejaVu Sans Mono,Liberation Mono,monospace 13");
    a->font_bold = pango_font_description_copy(a->font);
    pango_font_description_set_weight(a->font_bold, PANGO_WEIGHT_BOLD);
    PangoLayout *l = gtk_widget_create_pango_layout(a->win, "MMMMMMMMMMMMMMMMMMMM");
    pango_layout_set_font_description(l, a->font);
    PangoRectangle lr;
    pango_layout_get_extents(l, NULL, &lr);
    a->cell_w = (double)lr.width / PANGO_SCALE / 20.0;
    a->cell_h = (double)((lr.height + PANGO_SCALE - 1) / PANGO_SCALE);
    if (a->cell_w < 4) a->cell_w = 8;
    if (a->cell_h < 8) a->cell_h = 16;
    g_object_unref(l);
}

static GtkWidget *top_button(const char *label, const char *tip, GCallback cb, App *a)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "tbtn");
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_can_focus(b, FALSE);                    /* keep keyboard focus in the terminal */
    g_signal_connect(b, "clicked", cb, a);
    return b;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    g_setenv("GIT_OPTIONAL_LOCKS", "0", TRUE);      /* status polling must never lock the repo */
    App *a = g_new0(App, 1);

    struct passwd *pw = getpwuid(getuid());
    g_strlcpy(a->user, pw ? pw->pw_name : "user", sizeof a->user);
    g_strlcpy(a->home, g_get_home_dir(), sizeof a->home);
    gethostname(a->host, sizeof a->host);
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) g_strlcpy(cwd, a->home, sizeof cwd);

    char *hp = g_build_filename(a->home, HIST_DIR_NAME, HIST_FILE_NAME, NULL);
    hist_open(&a->hist, hp, HIST_MAX_ENTRIES, HIST_MAX_BYTES);
    g_free(hp);
    a->si = sysinfo_new();

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    a->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(a->win), "hterm");
    gtk_window_set_default_size(GTK_WINDOW(a->win), 1500, 940);
    g_signal_connect(a->win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(a->win, "key-press-event", G_CALLBACK(on_window_key), a);   /* pane shortcuts */
    measure_font(a);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(a->win), root);

    /* top bar */
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(top), "topbar");
    gtk_box_pack_start(GTK_BOX(top), label_new("▣  Terminal", "tab"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), top_button("⫼ Split right", "Split right  (⌘D / Ctrl+Shift+R)", G_CALLBACK(on_btn_split_right), a), FALSE, FALSE, 12);
    gtk_box_pack_start(GTK_BOX(top), top_button("⊟ Split below", "Split below  (⌘⇧D / Ctrl+Shift+D)", G_CALLBACK(on_btn_split_below), a), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), top_button("✕ Close pane", "Close pane  (⌘W / Ctrl+Shift+W)", G_CALLBACK(on_btn_close), a), FALSE, FALSE, 0);
    a->clock_lbl = label_new("", "tab");
    gtk_box_pack_end(GTK_BOX(top), a->clock_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), top, FALSE, FALSE, 0);

    /* status bar widget must exist before the first prompt updates it */
    a->status = label_new("", "status");
    gtk_style_context_add_class(gtk_widget_get_style_context(a->status), "status");

    load_custom_scene(a);
    GtkWidget *dash = build_dashboard(a);                      /* explorer / git widgets used by prompts */
    scene_set(a, SC_FIRE);

    a->pane_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    Term *t = term_new(a, cwd, TRUE);
    gtk_box_pack_start(GTK_BOX(a->pane_area), t->root, TRUE, TRUE, 0);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(paned), a->pane_area, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned), dash, FALSE, FALSE);
    gtk_paned_set_position(GTK_PANED(paned), 780);                 /* ≈52 / 48 split */
    gtk_box_pack_start(GTK_BOX(root), paned, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), a->status, FALSE, FALSE, 0);

    a->cur = t;
    gtk_style_context_add_class(gtk_widget_get_style_context(t->root), "pane-active");
    refresh_git(a);
    refresh_explorer(a);
    update_dashboard(a);
    update_status(a);
    g_timeout_add_seconds(1, on_tick, a);
    g_timeout_add(80, scene_tick, a);                  /* pixel-scene animation */

    gtk_widget_show_all(a->win);
    gtk_widget_grab_focus(t->tv);
    gtk_main();

    for (GList *l = a->terms; l; l = l->next) {
        Term *x = l->data;
        if (x->vt) vt_hangup(x->vt);
    }
    hist_close(&a->hist);
    free(a->si);
    return 0;
}
