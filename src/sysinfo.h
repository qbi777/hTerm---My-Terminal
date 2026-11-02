#ifndef HTERM_SYSINFO_H
#define HTERM_SYSINFO_H

#define SI_MAX_CORES 64
#define SI_MAX_PROCS 2048

typedef struct { unsigned long long total, idle; } CpuTicks;

typedef struct {
    int pid;
    char name[32];
    char state;                 /* R, S, D, T, Z ... */
    double cpu, mem;            /* percent */
    unsigned long long ticks;   /* utime + stime */
} ProcInfo;

typedef struct {
    int ncores;
    double cpu_total, cpu_core[SI_MAX_CORES];
    double mem_pct, swap_pct;
    double mem_used_gb, mem_total_gb, swap_used_gb, swap_total_gb;
    int nprocs, nrunning;
    ProcInfo procs[SI_MAX_PROCS];   /* sorted: cpu desc, mem desc */
    /* internals */
    CpuTicks prev_total, prev_core[SI_MAX_CORES];
    ProcInfo prev[SI_MAX_PROCS];
    int nprev, have_prev;
    unsigned long long prev_wall;   /* macOS: wall-clock ns at last sample */
} SysInfo;

SysInfo *sysinfo_new(void);                 /* heap: struct is large */
int      sysinfo_update(SysInfo *s);        /* call once per second  */

#endif
