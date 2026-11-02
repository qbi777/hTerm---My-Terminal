#ifndef HTERM_VT_H
#define HTERM_VT_H
/* Minimal xterm-style terminal emulator + PTY launcher.
 * Enough for vi/vim/nvim, nano, less, man, top/htop, ssh, python REPL...
 * No GTK in here, so it can be unit-tested from the command line.            */
#include <stdint.h>
#include <sys/types.h>

enum { VT_BOLD = 1, VT_DIM = 2, VT_UL = 4, VT_REV = 8, VT_ITALIC = 16 };

typedef struct {
    uint32_t ch;        /* unicode code point (' ' when empty)                 */
    uint32_t fg, bg;    /* 0 = default colour, else (1<<24)|0xRRGGBB            */
    uint8_t  attr;
} VtCell;

typedef struct Vt {
    int rows, cols;
    VtCell *main_buf, *alt_buf, *cells;     /* cells -> the active buffer      */
    int alt, ever_alt;                       /* ever_alt: app used the alt screen */
    int cx, cy, wrap_next;
    int top, bot;                            /* scroll region (inclusive)       */
    uint32_t fg, bg; uint8_t attr;           /* current pen                     */
    int saved_cx, saved_cy; uint32_t saved_fg, saved_bg; uint8_t saved_attr;
    int cursor_visible, app_cursor, autowrap, bracketed_paste, g0_graphics;
    /* parser */
    int state, which;
    int par[16], npar, priv, inter;
    uint32_t cp; int utf_need;
    char osc[256]; int osclen, osc_collect;
    char title[128]; int title_changed;
    /* pty */
    int master; pid_t pid;
} Vt;

Vt  *vt_new(int rows, int cols);
void vt_free(Vt *v);
int  vt_spawn(Vt *v, const char *cwd, char *const argv[]);   /* 0 ok, -1 error   */
void vt_resize(Vt *v, int rows, int cols);
void vt_feed(Vt *v, const char *buf, size_t n);              /* parse output     */
int  vt_read(Vt *v);               /* >=0 bytes consumed, -1 = child closed pty  */
void vt_write(Vt *v, const char *buf, size_t n);             /* keystrokes → child */
void vt_hangup(Vt *v);                                       /* SIGHUP the child   */
int  vt_row_text(const Vt *v, int row, char *out, size_t n); /* for tests          */

#endif
