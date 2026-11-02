/* macOS backend for sysinfo.h (Mach + libproc). Per-process data is limited to
 * processes your user owns unless hterm runs as root - same as `top` without sudo. */
#include "sysinfo.h"
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <time.h>

SysInfo *sysinfo_new(void) { return calloc(1, sizeof(SysInfo)); }

static double pct(CpuTicks *prev, const CpuTicks *now)
{
    double dt = (double)(now->total - prev->total);
    double di = (double)(now->idle  - prev->idle);
    double r  = dt > 0 ? 100.0 * (dt - di) / dt : 0.0;
    *prev = *now;
    return r < 0 ? 0 : r > 100 ? 100 : r;
}

static void read_cpu(SysInfo *s)
{
    natural_t n = 0; processor_info_array_t info = NULL; mach_msg_type_number_t cnt = 0;
    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &n, &info, &cnt) != KERN_SUCCESS)
        return;
    processor_cpu_load_info_data_t *l = (processor_cpu_load_info_data_t *)info;
    s->ncores = n > SI_MAX_CORES ? SI_MAX_CORES : (int)n;
    CpuTicks agg = { 0, 0 };
    for (int i = 0; i < s->ncores; i++) {
        unsigned long long u  = l[i].cpu_ticks[CPU_STATE_USER],
                           sy = l[i].cpu_ticks[CPU_STATE_SYSTEM],
                           id = l[i].cpu_ticks[CPU_STATE_IDLE],
                           ni = l[i].cpu_ticks[CPU_STATE_NICE];
        CpuTicks now = { u + sy + id + ni, id };
        agg.total += now.total; agg.idle += now.idle;
        s->cpu_core[i] = pct(&s->prev_core[i], &now);
    }
    s->cpu_total = pct(&s->prev_total, &agg);
    vm_deallocate(mach_task_self(), (vm_address_t)info, (vm_size_t)cnt * sizeof(integer_t));
}

static double read_mem(SysInfo *s)            /* returns total RAM in bytes */
{
    uint64_t total = 0; size_t len = sizeof total;
    sysctlbyname("hw.memsize", &total, &len, NULL, 0);

    vm_statistics64_data_t vm; memset(&vm, 0, sizeof vm);
    mach_msg_type_number_t c = HOST_VM_INFO64_COUNT;
    host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &c);
    vm_size_t ps = 0; host_page_size(mach_host_self(), &ps);

    double used = ((double)vm.active_count + vm.wire_count + vm.compressor_page_count) * (double)ps;
    s->mem_total_gb = (double)total / 1073741824.0;
    s->mem_used_gb  = used / 1073741824.0;
    s->mem_pct      = total ? 100.0 * used / (double)total : 0;

    struct xsw_usage xsw; len = sizeof xsw; memset(&xsw, 0, sizeof xsw);
    sysctlbyname("vm.swapusage", &xsw, &len, NULL, 0);
    s->swap_total_gb = (double)xsw.xsu_total / 1073741824.0;
    s->swap_used_gb  = (double)xsw.xsu_used  / 1073741824.0;
    s->swap_pct      = xsw.xsu_total ? 100.0 * (double)xsw.xsu_used / (double)xsw.xsu_total : 0;
    return (double)total;
}

static int cmp_proc(const void *a, const void *b)
{
    const ProcInfo *x = a, *y = b;
    if (x->cpu != y->cpu) return x->cpu < y->cpu ? 1 : -1;
    if (x->mem != y->mem) return x->mem < y->mem ? 1 : -1;
    return x->pid - y->pid;
}

static void read_procs(SysInfo *s, double total_mem)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    unsigned long long wall = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    unsigned long long dwall = s->prev_wall ? wall - s->prev_wall : 0;

    int bytes = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (bytes <= 0) return;
    int *pids = malloc((size_t)bytes + 64 * sizeof(int));
    if (!pids) return;
    bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, bytes + 64 * (int)sizeof(int));
    int n = bytes > 0 ? bytes / (int)sizeof(int) : 0;

    s->nprocs = s->nrunning = 0;
    for (int i = 0; i < n && s->nprocs < SI_MAX_PROCS; i++) {
        if (pids[i] <= 0) continue;
        struct proc_taskinfo ti;
        if (proc_pidinfo(pids[i], PROC_PIDTASKINFO, 0, &ti, sizeof ti) != (int)sizeof ti)
            continue;                                   /* not ours / no permission */
        ProcInfo *p = &s->procs[s->nprocs];
        memset(p, 0, sizeof *p);
        p->pid = pids[i];
        proc_name(pids[i], p->name, sizeof p->name);
        if (!p->name[0]) strncpy(p->name, "?", sizeof p->name - 1);

        struct proc_bsdinfo bi; p->state = 'S';
        if (proc_pidinfo(pids[i], PROC_PIDTBSDINFO, 0, &bi, sizeof bi) == (int)sizeof bi) {
            switch (bi.pbi_status) {
            case SRUN: p->state = 'R'; break;
            case SSTOP: p->state = 'T'; break;
            case SZOMB: p->state = 'Z'; break;
            default: p->state = 'S';
            }
        }
        p->ticks = (ti.pti_total_user + ti.pti_total_system) * tb.numer / tb.denom;   /* ns */
        p->mem   = total_mem > 0 ? 100.0 * (double)ti.pti_resident_size / total_mem : 0;
        if (s->have_prev && dwall) {
            for (int j = 0; j < s->nprev; j++)
                if (s->prev[j].pid == p->pid) {
                    p->cpu = 100.0 * (double)(p->ticks - s->prev[j].ticks) / (double)dwall;
                    break;
                }
        }
        if (p->state == 'R') s->nrunning++;
        s->nprocs++;
    }
    free(pids);

    memcpy(s->prev, s->procs, (size_t)s->nprocs * sizeof(ProcInfo));
    s->nprev = s->nprocs; s->have_prev = 1; s->prev_wall = wall;
    qsort(s->procs, (size_t)s->nprocs, sizeof(ProcInfo), cmp_proc);
}

int sysinfo_update(SysInfo *s)
{
    read_cpu(s);
    double total = read_mem(s);
    read_procs(s, total);
    return 0;
}
