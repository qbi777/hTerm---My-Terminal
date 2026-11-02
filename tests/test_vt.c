#define _GNU_SOURCE
#include "../src/vt.h"
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static char buf[1024];
static const char *row(Vt *v, int r) { vt_row_text(v, r, buf, sizeof buf); return buf; }
#define FEED(v, s) vt_feed(v, s, strlen(s))

static void test_sequences(void)
{
    Vt *v = vt_new(5, 10);
    FEED(v, "hello\r\nworld");
    assert(!strcmp(row(v, 0), "hello") && !strcmp(row(v, 1), "world"));

    FEED(v, "\033[2J\033[3;5Hxy");                       /* clear + cursor address */
    assert(!strcmp(row(v, 0), "") && !strcmp(row(v, 2), "    xy"));

    FEED(v, "\033[1;1H\033[31;1mR\033[0mn");              /* SGR colour + bold      */
    assert(v->cells[0].fg == ((1u << 24) | 0xff5c7a) && (v->cells[0].attr & VT_BOLD));
    assert(v->cells[1].fg == 0 && v->cells[1].attr == 0);
    FEED(v, "\033[38;2;1;2;3mT\033[38;5;196mU");           /* truecolor + 256        */
    assert(v->cells[2].fg == ((1u << 24) | 0x010203) && v->cells[3].fg == ((1u << 24) | 0xff0000));

    FEED(v, "\033[2J\033[1;1H0123456789AB");               /* autowrap at col 10     */
    assert(!strcmp(row(v, 0), "0123456789") && !strcmp(row(v, 1), "AB"));

    FEED(v, "\033[2J\033[1;1H\xc3\xa9\xe2\x82\xac");       /* UTF-8: é €             */
    assert(!strcmp(row(v, 0), "\xc3\xa9\xe2\x82\xac"));

    FEED(v, "\033[2J\033[1;1Habcdef\033[1;3H\033[2P");     /* delete 2 chars         */
    assert(!strcmp(row(v, 0), "abef"));
    FEED(v, "\033[1;3H\033[2@");                            /* insert 2 blanks        */
    assert(!strcmp(row(v, 0), "ab  ef"));

    FEED(v, "\033[2J\033[1;1HL1\r\nL2\r\nL3\r\nL4\r\nL5");  /* scroll region + scroll */
    FEED(v, "\033[2;4r\033[4;1H\n");
    assert(!strcmp(row(v, 0), "L1") && !strcmp(row(v, 1), "L3") && !strcmp(row(v, 3), "") && !strcmp(row(v, 4), "L5"));
    FEED(v, "\033[r");

    FEED(v, "\033[?1049h\033[1;1Halt");                    /* alternate screen       */
    assert(v->alt && !strcmp(row(v, 0), "alt"));
    FEED(v, "\033[?1049l");
    assert(!v->alt && !strcmp(row(v, 0), "L1"));            /* main screen restored   */

    FEED(v, "\033[2J\033[1;1H\033(0lqk\033(B");             /* DEC line drawing       */
    assert(!strcmp(row(v, 0), "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x90"));

    FEED(v, "\033]0;my title\007");
    assert(v->title_changed && !strcmp(v->title, "my title"));
    FEED(v, "\033[?1h"); assert(v->app_cursor);
    FEED(v, "\033[?25l"); assert(!v->cursor_visible);

    vt_resize(v, 8, 20);
    assert(v->rows == 8 && v->cols == 20);
    vt_free(v);
    printf("vt: escape-sequence tests passed\n");
}

/* Run a real full-screen curses program on a PTY and interact with it. */
static int pump(Vt *v, int ms)
{
    struct pollfd p = { v->master, POLLIN, 0 };
    int r = poll(&p, 1, ms);
    if (r > 0) return vt_read(v);
    return 0;
}

static void test_pty(void)
{
    const char *py =
        "import curses,sys\n"
        "def main(s):\n"
        "    curses.curs_set(1)\n"
        "    s.addstr(0,0,'-- EDITOR --',curses.A_REVERSE)\n"
        "    buf=''\n"
        "    while True:\n"
        "        k=s.getch()\n"
        "        if k==27: break\n"
        "        if k in (10,13): buf+='\\n'\n"
        "        elif k==curses.KEY_UP: buf+='^'\n"
        "        else: buf+=chr(k)\n"
        "        s.addstr(2,0,' '*40); s.addstr(2,0,buf.replace('\\n','|'))\n"
        "        s.refresh()\n"
        "    open(sys.argv[1],'w').write(buf)\n"
        "curses.wrapper(main)\n";
    char tmp[] = "/tmp/hterm_vtXXXXXX";
    int fd = mkstemp(tmp); assert(fd >= 0); close(fd);

    Vt *v = vt_new(24, 80);
    char *argv[] = { "python3", "-c", (char *)py, tmp, NULL };
    assert(vt_spawn(v, "/tmp", argv) == 0);

    for (int i = 0; i < 30 && strncmp(row(v, 0), "-- EDITOR --", 12); i++) pump(v, 100);
    assert(!strncmp(row(v, 0), "-- EDITOR --", 12));          /* curses drew its UI     */
    assert(v->alt);                                           /* in the alternate screen*/
    assert(v->cells[0].attr & VT_REV);                        /* reverse video worked   */

    vt_write(v, "hi\r", 3);
    assert(v->app_cursor);                                    /* curses enabled keypad mode */
    vt_write(v, v->app_cursor ? "\033OA" : "\033[A", 3);     /* arrow key, as the GUI encodes it */
    for (int i = 0; i < 10; i++) pump(v, 100);
    assert(strstr(row(v, 2), "hi|") != NULL);
    vt_write(v, "\033", 1);                                   /* ESC → program saves    */

    int st = 0;
    for (int i = 0; i < 50; i++) { pump(v, 100); if (waitpid(v->pid, &st, WNOHANG) > 0) break; }
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    for (int i = 0; i < 5; i++) pump(v, 50);                  /* drain the exit sequences */
    assert(!v->alt);                                          /* left alt screen        */

    FILE *f = fopen(tmp, "r"); assert(f);
    char out[64] = ""; size_t n = fread(out, 1, sizeof out - 1, f); out[n] = '\0'; fclose(f);
    assert(!strcmp(out, "hi\n^"));                            /* keys reached the program */
    unlink(tmp);
    vt_free(v);
    printf("vt: PTY + curses full-screen test passed\n");
}

int main(void) { test_sequences(); test_pty(); return 0; }
