#include "../src/sysinfo.h"
#include <stdio.h>
#include <unistd.h>
int main(void)
{
    SysInfo *s = sysinfo_new();
    sysinfo_update(s); sleep(1); sysinfo_update(s);
    printf("cores=%d cpu=%.1f%% mem=%.1f/%.1fG swap=%.1f%%  procs=%d running=%d\n",
           s->ncores, s->cpu_total, s->mem_used_gb, s->mem_total_gb, s->swap_pct,
           s->nprocs, s->nrunning);
    for (int i = 0; i < 5 && i < s->nprocs; i++)
        printf("  %6d %-16s %c cpu=%.1f mem=%.1f\n", s->procs[i].pid, s->procs[i].name,
               s->procs[i].state, s->procs[i].cpu, s->procs[i].mem);
    return s->nprocs > 0 && s->ncores > 0 ? 0 : 1;
}
