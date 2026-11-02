#define _GNU_SOURCE
#include "vt.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

enum { S_GROUND, S_ESC, S_CSI, S_OSC, S_OSC_ESC, S_CHARSET, S_SKIP1 };

#define CELL(v, r, c) ((v)->cells[(size_t)(r) * (size_t)(v)->cols + (size_t)(c)])
#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))

/* ---------- colours ----------------------------------------------------- */

static const uint32_t ANSI16[16] = {
    0x1b2438, 0xff5c7a, 0x00e5a0, 0xffb454, 0x3aa0ff, 0xc792ea, 0x17b8d8, 0xc8e8ff,
    0x5b7fa0, 0xff8099, 0x4dffc3, 0xffd080, 0x7ab8ff, 0xdcb0ff, 0x5fd8f0, 0xffffff };

static uint32_t palette(int i)
{
    uint32_t rgb;
    if (i < 16) rgb = ANSI16[i];
    else if (i < 232) {
        static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
        int n = i - 16;
        rgb = (uint32_t)(lv[n / 36] << 16 | lv[(n / 6) % 6] << 8 | lv[n % 6]);
    } else { int g = 8 + (i - 232) * 10; rgb = (uint32_t)(g << 16 | g << 8 | g); }
    return (1u << 24) | rgb;
}

/* ---------- screen primitives ------------------------------------------- */

static VtCell blank(const Vt *v) { VtCell c = { ' ', 0, v->bg, 0 }; return c; }

static void fill(Vt *v, int r0, int c0, int r1, int c1)       /* inclusive */
{
    VtCell b = blank(v);
    for (int r = r0; r <= r1; r++)
        for (int c = c0; c <= c1; c++) CELL(v, r, c) = b;
}

static void scroll_up(Vt *v, int top, int bot, int n)
{
    if (n > bot - top + 1) n = bot - top + 1;
    if (n <= 0) return;
    memmove(&CELL(v, top, 0), &CELL(v, top + n, 0),
            (size_t)(bot - top + 1 - n) * (size_t)v->cols * sizeof(VtCell));
    fill(v, bot - n + 1, 0, bot, v->cols - 1);
}

static void scroll_down(Vt *v, int top, int bot, int n)
{
    if (n > bot - top + 1) n = bot - top + 1;
    if (n <= 0) return;
    memmove(&CELL(v, top + n, 0), &CELL(v, top, 0),
            (size_t)(bot - top + 1 - n) * (size_t)v->cols * sizeof(VtCell));
    fill(v, top, 0, top + n - 1, v->cols - 1);
}

static void linefeed(Vt *v)
{
    if (v->cy == v->bot) scroll_up(v, v->top, v->bot, 1);
    else if (v->cy < v->rows - 1) v->cy++;
}

static void reverse_index(Vt *v)
{
    if (v->cy == v->top) scroll_down(v, v->top, v->bot, 1);
    else if (v->cy > 0) v->cy--;
}

static const uint32_t GFX[32] = {            /* DEC special graphics 0x5f..0x7e */
    0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, 0x2424, 0x240B,
    0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C,
    0x2524, 0x2534, 0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7 };

static void put_char(Vt *v, uint32_t cp)
{
    if (v->g0_graphics && cp >= 0x5f && cp <= 0x7e) cp = GFX[cp - 0x5f];
    if (v->wrap_next && v->autowrap) { v->cx = 0; linefeed(v); }
    v->wrap_next = 0;
    VtCell *c = &CELL(v, v->cy, v->cx);
    c->ch = cp; c->fg = v->fg; c->bg = v->bg; c->attr = v->attr;
    if (v->cx == v->cols - 1) v->wrap_next = 1; else v->cx++;
}

static void set_alt(Vt *v, int on, int save)
{
    if (on == v->alt) return;
    if (on) {
        if (save) { v->saved_cx = v->cx; v->saved_cy = v->cy; v->saved_fg = v->fg;
                    v->saved_bg = v->bg; v->saved_attr = v->attr; }
        v->alt = 1; v->ever_alt = 1; v->cells = v->alt_buf;
        v->fg = v->bg = 0; v->attr = 0;
        fill(v, 0, 0, v->rows - 1, v->cols - 1);
        v->cx = v->cy = 0;
    } else {
        v->alt = 0; v->cells = v->main_buf;
        if (save) { v->cx = CLAMP(v->saved_cx, 0, v->cols - 1); v->cy = CLAMP(v->saved_cy, 0, v->rows - 1);
                    v->fg = v->saved_fg; v->bg = v->saved_bg; v->attr = v->saved_attr; }
    }
    v->wrap_next = 0; v->top = 0; v->bot = v->rows - 1;
}

static void reply(Vt *v, const char *s) { vt_write(v, s, strlen(s)); }

/* ---------- CSI --------------------------------------------------------- */

#define P(i, def) ((i) < v->npar && v->par[i] > 0 ? v->par[i] : (def))

static void sgr(Vt *v)
{
    if (v->npar == 0) { v->fg = v->bg = 0; v->attr = 0; return; }
    for (int i = 0; i < v->npar; i++) {
        int c = v->par[i];
        if (c == 0) { v->fg = v->bg = 0; v->attr = 0; }
        else if (c == 1) v->attr |= VT_BOLD;
        else if (c == 2) v->attr |= VT_DIM;
        else if (c == 3) v->attr |= VT_ITALIC;
        else if (c == 4) v->attr |= VT_UL;
        else if (c == 7) v->attr |= VT_REV;
        else if (c == 22) v->attr &= (uint8_t)~(VT_BOLD | VT_DIM);
        else if (c == 23) v->attr &= (uint8_t)~VT_ITALIC;
        else if (c == 24) v->attr &= (uint8_t)~VT_UL;
        else if (c == 27) v->attr &= (uint8_t)~VT_REV;
        else if (c >= 30 && c <= 37) v->fg = palette(c - 30);
        else if (c == 39) v->fg = 0;
        else if (c >= 40 && c <= 47) v->bg = palette(c - 40);
        else if (c == 49) v->bg = 0;
        else if (c >= 90 && c <= 97) v->fg = palette(c - 90 + 8);
        else if (c >= 100 && c <= 107) v->bg = palette(c - 100 + 8);
        else if (c == 38 || c == 48) {
            uint32_t col = 0;
            if (i + 2 < v->npar && v->par[i + 1] == 5) { col = palette(CLAMP(v->par[i + 2], 0, 255)); i += 2; }
            else if (i + 4 < v->npar && v->par[i + 1] == 2) {
                col = (1u << 24) | (uint32_t)(CLAMP(v->par[i + 2], 0, 255) << 16 |
                      CLAMP(v->par[i + 3], 0, 255) << 8 | CLAMP(v->par[i + 4], 0, 255));
                i += 4;
            } else break;
            if (c == 38) v->fg = col; else v->bg = col;
        }
    }
}

static void dec_mode(Vt *v, int set)
{
    for (int i = 0; i < v->npar; i++)
        switch (v->par[i]) {
        case 1:    v->app_cursor = set; break;
        case 7:    v->autowrap = set; break;
        case 25:   v->cursor_visible = set; break;
        case 47: case 1047: set_alt(v, set, 0); break;
        case 1049: set_alt(v, set, 1); break;
        case 2004: v->bracketed_paste = set; break;
        default: break;
        }
}

static void csi(Vt *v, int f)
{
    int n;
    switch (f) {
    case 'A': v->cy = CLAMP(v->cy - P(0, 1), 0, v->rows - 1); v->wrap_next = 0; break;
    case 'B': case 'e': v->cy = CLAMP(v->cy + P(0, 1), 0, v->rows - 1); v->wrap_next = 0; break;
    case 'C': case 'a': v->cx = CLAMP(v->cx + P(0, 1), 0, v->cols - 1); v->wrap_next = 0; break;
    case 'D': v->cx = CLAMP(v->cx - P(0, 1), 0, v->cols - 1); v->wrap_next = 0; break;
    case 'E': v->cy = CLAMP(v->cy + P(0, 1), 0, v->rows - 1); v->cx = 0; v->wrap_next = 0; break;
    case 'F': v->cy = CLAMP(v->cy - P(0, 1), 0, v->rows - 1); v->cx = 0; v->wrap_next = 0; break;
    case 'G': case '`': v->cx = CLAMP(P(0, 1) - 1, 0, v->cols - 1); v->wrap_next = 0; break;
    case 'd': v->cy = CLAMP(P(0, 1) - 1, 0, v->rows - 1); v->wrap_next = 0; break;
    case 'H': case 'f':
        v->cy = CLAMP(P(0, 1) - 1, 0, v->rows - 1);
        v->cx = CLAMP(P(1, 1) - 1, 0, v->cols - 1);
        v->wrap_next = 0; break;
    case 'J':
        n = v->npar ? v->par[0] : 0;
        if (n == 0) { fill(v, v->cy, v->cx, v->cy, v->cols - 1);
                      if (v->cy + 1 < v->rows) fill(v, v->cy + 1, 0, v->rows - 1, v->cols - 1); }
        else if (n == 1) { if (v->cy > 0) fill(v, 0, 0, v->cy - 1, v->cols - 1);
                           fill(v, v->cy, 0, v->cy, v->cx); }
        else fill(v, 0, 0, v->rows - 1, v->cols - 1);
        break;
    case 'K':
        n = v->npar ? v->par[0] : 0;
        if (n == 0) fill(v, v->cy, v->cx, v->cy, v->cols - 1);
        else if (n == 1) fill(v, v->cy, 0, v->cy, v->cx);
        else fill(v, v->cy, 0, v->cy, v->cols - 1);
        break;
    case 'L': if (v->cy >= v->top && v->cy <= v->bot) scroll_down(v, v->cy, v->bot, P(0, 1)); break;
    case 'M': if (v->cy >= v->top && v->cy <= v->bot) scroll_up(v, v->cy, v->bot, P(0, 1)); break;
    case '@': {
        n = CLAMP(P(0, 1), 1, v->cols - v->cx);
        memmove(&CELL(v, v->cy, v->cx + n), &CELL(v, v->cy, v->cx),
                (size_t)(v->cols - v->cx - n) * sizeof(VtCell));
        fill(v, v->cy, v->cx, v->cy, v->cx + n - 1); break; }
    case 'P': {
        n = CLAMP(P(0, 1), 1, v->cols - v->cx);
        memmove(&CELL(v, v->cy, v->cx), &CELL(v, v->cy, v->cx + n),
                (size_t)(v->cols - v->cx - n) * sizeof(VtCell));
        fill(v, v->cy, v->cols - n, v->cy, v->cols - 1); break; }
    case 'X': n = CLAMP(P(0, 1), 1, v->cols - v->cx); fill(v, v->cy, v->cx, v->cy, v->cx + n - 1); break;
    case 'S': scroll_up(v, v->top, v->bot, P(0, 1)); break;
    case 'T': scroll_down(v, v->top, v->bot, P(0, 1)); break;
    case 'm': if (!v->priv) sgr(v); break;
    case 'r': {
        int t = P(0, 1) - 1, b = P(1, v->rows) - 1;
        if (t < 0) t = 0;
        if (b >= v->rows) b = v->rows - 1;
        if (t < b) { v->top = t; v->bot = b; }
        v->cx = v->cy = 0; v->wrap_next = 0; break; }
    case 's': v->saved_cx = v->cx; v->saved_cy = v->cy; break;
    case 'u': v->cx = CLAMP(v->saved_cx, 0, v->cols - 1); v->cy = CLAMP(v->saved_cy, 0, v->rows - 1); break;
    case 'h': if (v->priv == '?') dec_mode(v, 1); break;
    case 'l': if (v->priv == '?') dec_mode(v, 0); break;
    case 'n':
        if (v->npar && v->par[0] == 6) { char b[32]; snprintf(b, sizeof b, "\033[%d;%dR", v->cy + 1, v->cx + 1); reply(v, b); }
        else if (v->npar && v->par[0] == 5) reply(v, "\033[0n");
        break;
    case 'c': if (!v->priv) reply(v, "\033[?6c"); break;
    default: break;                                          /* ignore the rest */
    }
}

/* ---------- byte stream ------------------------------------------------- */

static void control(Vt *v, unsigned char b)
{
    switch (b) {
    case 0x08: if (v->cx > 0) v->cx--; v->wrap_next = 0; break;
    case 0x09: v->cx = CLAMP((v->cx / 8 + 1) * 8, 0, v->cols - 1); break;
    case 0x0a: case 0x0b: case 0x0c: v->wrap_next = 0; linefeed(v); break;
    case 0x0d: v->cx = 0; v->wrap_next = 0; break;
    default: break;                                          /* BEL, SO, SI ... */
    }
}

static void osc_done(Vt *v)
{
    if (v->osc_collect && v->osclen > 2 && v->osc[1] == ';' && (v->osc[0] == '0' || v->osc[0] == '2')) {
        v->osc[v->osclen] = '\0';
        snprintf(v->title, sizeof v->title, "%s", v->osc + 2);
        v->title_changed = 1;
    }
    v->state = S_GROUND;
}

void vt_feed(Vt *v, const char *buf, size_t n)
{
    for (size_t k = 0; k < n; k++) {
        unsigned char b = (unsigned char)buf[k];
    again:
        switch (v->state) {
        case S_GROUND:
            if (v->utf_need) {
                if ((b & 0xC0) == 0x80) {
                    v->cp = (v->cp << 6) | (b & 0x3F);
                    if (--v->utf_need == 0) put_char(v, v->cp);
                    continue;
                }
                v->utf_need = 0; put_char(v, 0xFFFD);
            }
            if (b == 0x1b) v->state = S_ESC;
            else if (b < 0x20) control(v, b);
            else if (b == 0x7f) { }
            else if (b >= 0xF0) { v->cp = b & 0x07; v->utf_need = 3; }
            else if (b >= 0xE0) { v->cp = b & 0x0F; v->utf_need = 2; }
            else if (b >= 0xC0) { v->cp = b & 0x1F; v->utf_need = 1; }
            else if (b >= 0x80) put_char(v, 0xFFFD);
            else put_char(v, b);
            break;
        case S_ESC:
            v->state = S_GROUND;
            switch (b) {
            case '[': v->state = S_CSI; v->npar = 0; v->par[0] = 0; v->priv = 0; v->inter = 0; break;
            case ']': v->state = S_OSC; v->osclen = 0; v->osc_collect = 1; break;
            case 'P': case '_': case '^': case 'X': v->state = S_OSC; v->osclen = 0; v->osc_collect = 0; break;
            case '(': case ')': case '*': case '+': v->state = S_CHARSET; v->which = b; break;
            case '#': v->state = S_SKIP1; break;
            case '7': v->saved_cx = v->cx; v->saved_cy = v->cy; v->saved_fg = v->fg; v->saved_bg = v->bg; v->saved_attr = v->attr; break;
            case '8': v->cx = CLAMP(v->saved_cx, 0, v->cols - 1); v->cy = CLAMP(v->saved_cy, 0, v->rows - 1);
                      v->fg = v->saved_fg; v->bg = v->saved_bg; v->attr = v->saved_attr; break;
            case 'M': reverse_index(v); break;
            case 'D': linefeed(v); break;
            case 'E': v->cx = 0; linefeed(v); break;
            case 'c': set_alt(v, 0, 0); v->fg = v->bg = 0; v->attr = 0; v->top = 0; v->bot = v->rows - 1;
                      v->cursor_visible = 1; v->app_cursor = 0; v->autowrap = 1; v->g0_graphics = 0;
                      fill(v, 0, 0, v->rows - 1, v->cols - 1); v->cx = v->cy = 0; break;
            case 0x1b: v->state = S_ESC; break;
            default: break;
            }
            break;
        case S_CSI:
            if (b == 0x1b) { v->state = S_ESC; }
            else if (b < 0x20) control(v, b);
            else if (b >= '0' && b <= '9') {
                if (v->npar == 0) v->npar = 1;
                int *p = &v->par[v->npar - 1];
                if (*p < 100000) *p = *p * 10 + (b - '0');
            } else if (b == ';' || b == ':') {
                if (v->npar == 0) { v->npar = 1; v->par[0] = 0; }
                if (v->npar < 16) { v->npar++; v->par[v->npar - 1] = 0; }
            } else if (b >= '<' && b <= '?') v->priv = b;
            else if (b >= 0x20 && b <= 0x2f) v->inter = b;
            else if (b >= '@' && b <= '~') { if (!v->inter) csi(v, b); v->state = S_GROUND; }
            break;
        case S_OSC:
            if (b == 0x07) osc_done(v);
            else if (b == 0x1b) v->state = S_OSC_ESC;
            else if (v->osc_collect && v->osclen < (int)sizeof v->osc - 1) v->osc[v->osclen++] = (char)b;
            break;
        case S_OSC_ESC:
            osc_done(v);
            if (b != '\\') { v->state = S_ESC; goto again; }
            break;
        case S_CHARSET:
            if (v->which == '(') v->g0_graphics = (b == '0');
            v->state = S_GROUND;
            break;
        case S_SKIP1:
            v->state = S_GROUND;
            break;
        }
    }
}

/* ---------- lifecycle / pty --------------------------------------------- */

static VtCell *alloc_buf(const Vt *v, int rows, int cols)
{
    VtCell *b = malloc((size_t)rows * (size_t)cols * sizeof(VtCell));
    if (!b) return NULL;
    VtCell bl = blank(v);
    for (size_t i = 0; i < (size_t)rows * (size_t)cols; i++) b[i] = bl;
    return b;
}

Vt *vt_new(int rows, int cols)
{
    Vt *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->rows = rows < 2 ? 2 : rows; v->cols = cols < 2 ? 2 : cols;
    v->main_buf = alloc_buf(v, v->rows, v->cols);
    v->alt_buf  = alloc_buf(v, v->rows, v->cols);
    if (!v->main_buf || !v->alt_buf) { vt_free(v); return NULL; }
    v->cells = v->main_buf;
    v->top = 0; v->bot = v->rows - 1;
    v->cursor_visible = 1; v->autowrap = 1; v->master = -1;
    return v;
}

void vt_free(Vt *v)
{
    if (!v) return;
    if (v->master >= 0) close(v->master);
    free(v->main_buf); free(v->alt_buf); free(v);
}

void vt_resize(Vt *v, int rows, int cols)
{
    if (rows < 2) rows = 2;
    if (cols < 2) cols = 2;
    if (rows == v->rows && cols == v->cols) return;
    VtCell *nm = alloc_buf(v, rows, cols), *na = alloc_buf(v, rows, cols);
    if (!nm || !na) { free(nm); free(na); return; }
    int rc = rows < v->rows ? rows : v->rows, cc = cols < v->cols ? cols : v->cols;
    /* main screen keeps the bottom rows (shell behaviour), alt screen keeps the top */
    int off = v->cy >= rows ? v->cy - rows + 1 : 0;
    for (int r = 0; r < rc; r++) {
        memcpy(&nm[(size_t)r * cols], &v->main_buf[(size_t)(r + (v->alt ? 0 : off)) * v->cols], (size_t)cc * sizeof(VtCell));
        memcpy(&na[(size_t)r * cols], &v->alt_buf[(size_t)r * v->cols], (size_t)cc * sizeof(VtCell));
    }
    free(v->main_buf); free(v->alt_buf);
    v->main_buf = nm; v->alt_buf = na;
    v->cells = v->alt ? na : nm;
    v->rows = rows; v->cols = cols;
    v->top = 0; v->bot = rows - 1;
    v->cx = CLAMP(v->cx, 0, cols - 1); v->cy = CLAMP(v->cy - (v->alt ? 0 : off), 0, rows - 1);
    v->wrap_next = 0;
    if (v->master >= 0) {
        struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
        ioctl(v->master, TIOCSWINSZ, &ws);
    }
}

extern char **environ;

static const char *find_exe(const char *name, char *buf, size_t n)
{
    if (strchr(name, '/')) {
        snprintf(buf, n, "%s", name);
        return access(buf, X_OK) == 0 ? buf : NULL;
    }
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    char *copy = strdup(path), *save = NULL;
    if (!copy) return NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        snprintf(buf, n, "%s/%s", dir, name);
        if (access(buf, X_OK) == 0) { free(copy); return buf; }
    }
    free(copy);
    return NULL;
}

/* The child only calls chdir()/execve() (async-signal-safe): everything else is
 * prepared in the parent, which matters inside a multi-threaded GUI process. */
int vt_spawn(Vt *v, const char *cwd, char *const argv[])
{
    char exe[4096];
    if (!find_exe(argv[0], exe, sizeof exe)) { errno = ENOENT; return -1; }

    size_t n = 0;
    while (environ && environ[n]) n++;
    char **envp = calloc(n + 6, sizeof *envp);
    if (!envp) return -1;
    size_t k = 0;
    const char *lang = getenv("LANG");
    int utf = lang && strstr(lang, "UTF");
    for (size_t i = 0; i < n; i++) {
        if (!strncmp(environ[i], "TERM=", 5) || !strncmp(environ[i], "COLORTERM=", 10)) continue;
        if (!utf && (!strncmp(environ[i], "LANG=", 5) || !strncmp(environ[i], "LC_ALL=", 7))) continue;
        envp[k++] = environ[i];
    }
    envp[k++] = "TERM=xterm-256color";
    envp[k++] = "COLORTERM=truecolor";
    if (!utf) envp[k++] = "LANG=en_US.UTF-8";
    envp[k] = NULL;

    struct winsize ws = { (unsigned short)v->rows, (unsigned short)v->cols, 0, 0 };
    pid_t pid = forkpty(&v->master, NULL, NULL, &ws);
    if (pid < 0) { free(envp); return -1; }
    if (pid == 0) {
        if (cwd && chdir(cwd) != 0) { /* stay in the inherited directory */ }
        execve(exe, argv, envp);
        _exit(127);
    }
    free(envp);
    v->pid = pid;
    fcntl(v->master, F_SETFL, fcntl(v->master, F_GETFL) | O_NONBLOCK);
    fcntl(v->master, F_SETFD, FD_CLOEXEC);
    return 0;
}

int vt_read(Vt *v)
{
    char buf[8192];
    int total = 0;
    for (;;) {
        ssize_t n = read(v->master, buf, sizeof buf);
        if (n > 0) { vt_feed(v, buf, (size_t)n); total += (int)n; if (total > (1 << 18)) break; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        return total > 0 ? total : -1;                      /* EOF / EIO: child closed pty */
    }
    return total;
}

void vt_write(Vt *v, const char *buf, size_t n)
{
    if (v->master < 0) return;
    size_t off = 0; int tries = 0;
    while (off < n && tries < 50) {
        ssize_t w = write(v->master, buf + off, n - off);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = { v->master, POLLOUT, 0 };
            poll(&p, 1, 100); tries++; continue;
        }
        break;
    }
}

void vt_hangup(Vt *v) { if (v->pid > 0) kill(v->pid, SIGHUP); }

int vt_row_text(const Vt *v, int row, char *out, size_t n)
{
    size_t o = 0;
    for (int c = 0; c < v->cols && o + 5 < n; c++) {
        uint32_t cp = CELL(v, row, c).ch;
        if (cp < 0x80) out[o++] = (char)cp;
        else if (cp < 0x800) { out[o++] = (char)(0xC0 | cp >> 6); out[o++] = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out[o++] = (char)(0xE0 | cp >> 12); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
        else { out[o++] = (char)(0xF0 | cp >> 18); out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = '\0';
    return (int)o;
}
