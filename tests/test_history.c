#define _GNU_SOURCE
#include "../src/history.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    char dir[] = "/tmp/hterm_testXXXXXX";
    assert(mkdtemp(dir));
    char path[256];
    snprintf(path, sizeof path, "%s/sub/history.log", dir);

    History h;
    assert(hist_open(&h, path, 10, 600) == 0);

    /* blank / private / duplicate handling */
    hist_add(&h, "hubert", "/home/hubert", 0, "   ");
    hist_add(&h, "hubert", "/home/hubert", 0, " secret");
    hist_add(&h, "hubert", "/home/hubert", 0, "ls -la");
    hist_add(&h, "hubert", "/home/hubert", 0, "ls -la");
    assert(h.count == 1);

    /* flood: must stay within both limits */
    for (int i = 0; i < 200; i++) {
        char c[64]; snprintf(c, sizeof c, "echo line number %d\twith tab", i);
        hist_add(&h, "hubert", "/home/hubert/my projects", i % 3, c);
        struct stat sb; assert(stat(path, &sb) == 0);
        assert((size_t)sb.st_size <= 600);
        assert(h.count <= 10);
        assert((sb.st_mode & 0777) == 0600);
    }
    assert(strstr(h.e[h.count - 1].cmd, "199"));
    assert(!strchr(h.e[h.count - 1].cmd, '\t'));

    /* up/down navigation keeps the draft */
    const char *p = hist_prev(&h, "half typed");
    assert(strstr(p, "199"));
    p = hist_prev(&h, "ignored");
    assert(strstr(p, "198"));
    p = hist_next(&h); assert(strstr(p, "199"));
    p = hist_next(&h); assert(strcmp(p, "half typed") == 0);

    /* persistence: reopen and compare */
    size_t before = h.count;
    char last[128]; snprintf(last, sizeof last, "%s", h.e[h.count - 1].cmd);
    hist_close(&h);
    assert(hist_open(&h, path, 10, 600) == 0);
    assert(h.count == before);
    assert(strcmp(h.e[h.count - 1].cmd, last) == 0);
    assert(strcmp(h.e[0].user, "hubert") == 0);
    assert(strcmp(h.e[0].cwd, "/home/hubert/my projects") == 0);   /* spaces ok */

    /* shrinking the limit on reopen trims the file */
    hist_close(&h);
    assert(hist_open(&h, path, 3, 600) == 0);
    assert(h.count == 3 && h.file_lines == 3);

    assert(hist_clear(&h) == 0 && h.count == 0);
    hist_close(&h);
    printf("history: all tests passed\n");
    return 0;
}
