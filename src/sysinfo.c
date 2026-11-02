#define _GNU_SOURCE
#include "sysinfo.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

SysInfo *sysinfo_new(void) { return calloc(1, sizeof(SysInfo)); }

static double pct(CpuTicks *prev, const CpuTicks *now)
{
    double dt = (double)(now->total - prev->total);
    double di = (double)(now->idle  - prev->idle);
    double r  = dt > 0 ? 100.0 * (dt - di) / dt : 0.0;
    *prev = *now;
    return r < 0 ? 0 : r > 100 ? 100 : r;
}

static int read_cpu(SysInfo *s, unsigned long long *agg_delta)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    char line[512];
    int core = 0;
    while (fgets(line, sizeof line, f) && !strncmp(line, "cpu", 3)) {
        unsigned long long v[8] = {0};
        char *p = line + 3;
        int idx = -1;
        if (isdigit((unsigned char)*p)) idx = (int)strtol(p, &p, 10);
        sscanf(p, "%llu %llu %llu %llu %llu %llu %llu %llu",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
        CpuTicks now = { 0, v[3] + v[4] };
        for (int i = 0; i < 8; i++) now.total += v[i];
        if (idx < 0) {
            *agg_delta = now.total - s->prev_total.total;
            s->cpu_total = pct(&s->prev_total, &now);
        } else if (idx < SI_MAX_CORES) {
            s->cpu_core[idx] = pct(&s->prev_core[idx], &now);
            core = idx + 1;
        }
    }
    fclose(f);
    if (core) s->ncores = core;
    return 0;
}

static unsigned long long mem_kb(const char *key)
{
    static char buf[4096];
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    char *p = strstr(buf, key);
    return p ? strtoull(p + strlen(key), NULL, 10) : 0;
}

static void read_mem(SysInfo *s)
{
    double mt = (double)mem_kb("MemTotal:"), ma = (double)mem_kb("MemAvailable:");
    double st = (double)mem_kb("SwapTotal:"), sf = (double)mem_kb("SwapFree:");
    s->mem_total_gb  = mt / 1048576.0;
    s->mem_used_gb   = (mt - ma) / 1048576.0;
    s->mem_pct       = mt > 0 ? 100.0 * (mt - ma) / mt : 0;
    s->swap_total_gb = st / 1048576.0;
    s->swap_used_gb  = (st - sf) / 1048576.0;
    s->swap_pct      = st > 0 ? 100.0 * (st - sf) / st : 0;
}

static int cmp_proc(const void *a, const void *b)
{
    const ProcInfo *x = a, *y = b;
    if (x->cpu != y->cpu) return x->cpu < y->cpu ? 1 : -1;
    if (x->mem != y->mem) return x->mem < y->mem ? 1 : -1;
    return x->pid - y->pid;
}

static void read_procs(SysInfo *s, unsigned long long agg_delta)
{
    DIR *d = opendir("/proc");
    if (!d) return;
    double page_kb = sysconf(_SC_PAGESIZE) / 1024.0;
    double mt = s->mem_total_gb * 1048576.0;
    s->nprocs = s->nrunning = 0;

    struct dirent *de;
    while ((de = readdir(d)) && s->nprocs < SI_MAX_PROCS) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        char path[320], buf[1024];
        snprintf(path, sizeof path, "/proc/%s/stat", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = '\0';
        char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');   /* comm may contain ')' */
        if (!lp || !rp) continue;

        ProcInfo *p = &s->procs[s->nprocs];
        memset(p, 0, sizeof *p);
        p->pid = atoi(buf);
        size_t len = (size_t)(rp - lp - 1);
        if (len >= sizeof p->name) len = sizeof p->name - 1;
        memcpy(p->name, lp + 1, len);

        unsigned long ut = 0, st = 0; long rss = 0;
        if (sscanf(rp + 2, "%c %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s "
                           "%lu %lu %*s %*s %*s %*s %*s %*s %*s %*s %ld",
                   &p->state, &ut, &st, &rss) < 4) continue;
        p->ticks = (unsigned long long)ut + st;
        p->mem   = mt > 0 ? 100.0 * (double)rss * page_kb / mt : 0;

        if (s->have_prev && agg_delta) {
            for (int i = 0; i < s->nprev; i++)
                if (s->prev[i].pid == p->pid) {
                    unsigned long long dp = p->ticks - s->prev[i].ticks;
                    p->cpu = 100.0 * (double)dp * s->ncores / (double)agg_delta;
                    break;
                }
        }
        if (p->state == 'R') s->nrunning++;
        s->nprocs++;
    }
    closedir(d);

    memcpy(s->prev, s->procs, (size_t)s->nprocs * sizeof(ProcInfo));
    s->nprev = s->nprocs;
    s->have_prev = 1;
    qsort(s->procs, (size_t)s->nprocs, sizeof(ProcInfo), cmp_proc);
}

int sysinfo_update(SysInfo *s)
{
    unsigned long long agg = 0;
    if (read_cpu(s, &agg) != 0) return -1;
    read_mem(s);
    read_procs(s, agg);
    return 0;
}
