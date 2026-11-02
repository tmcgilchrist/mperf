/*
 * apple-perf-stat: A perf stat-like tool for macOS (Apple Silicon and Intel)
 *
 * Uses PET (Profile Every Thread) for accurate per-process counter totals.
 * Correctly handles multi-threaded programs like OCaml 5.x with multiple
 * domains, each having multiple pthreads.
 *
 * Based on ibireme's kpc_demo.c (public domain)
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/kdebug.h>
#include <mach/mach_time.h>
#include <sys/event.h>
#include <sys/proc_info.h>
#include <libproc.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <getopt.h>
#include <signal.h>
#include <errno.h>
#include <locale.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef int32_t i32;
typedef size_t usize;

// ============================================================================
// KPC/KPERF Framework Types and Function Pointers
// ============================================================================

#define KPC_CLASS_FIXED              (0)
#define KPC_CLASS_CONFIGURABLE       (1)
#define KPC_CLASS_FIXED_MASK         (1u << KPC_CLASS_FIXED)
#define KPC_CLASS_CONFIGURABLE_MASK  (1u << KPC_CLASS_CONFIGURABLE)
#define KPC_MAX_COUNTERS             32

// kperf sampler bits
#define KPERF_SAMPLER_TH_INFO       (1U << 0)
#define KPERF_SAMPLER_TH_SNAPSHOT   (1U << 1)
#define KPERF_SAMPLER_KSTACK        (1U << 2)
#define KPERF_SAMPLER_USTACK        (1U << 3)
#define KPERF_SAMPLER_PMC_THREAD    (1U << 4)
#define KPERF_SAMPLER_PMC_CPU       (1U << 5)
#define KPERF_SAMPLER_PMC_CONFIG    (1U << 6)
#define KPERF_SAMPLER_MEMINFO       (1U << 7)
#define KPERF_SAMPLER_TH_SCHEDULING (1U << 8)
#define KPERF_SAMPLER_TH_DISPATCH   (1U << 9)
#define KPERF_SAMPLER_TK_SNAPSHOT   (1U << 10)
#define KPERF_SAMPLER_SYS_MEM       (1U << 11)
#define KPERF_SAMPLER_TH_INSCYC     (1U << 12)
#define KPERF_SAMPLER_TK_INFO       (1U << 13)

#define KPERF_ACTION_MAX (32)
#define KPERF_TIMER_MAX  (8)

typedef u64 kpc_config_t;

// kperf.framework functions
static int (*kpc_force_all_ctrs_get)(int *val_out);
static int (*kpc_force_all_ctrs_set)(int val);
static u32 (*kpc_get_counter_count)(u32 classes);
static u32 (*kpc_get_config_count)(u32 classes);
static int (*kpc_set_config)(u32 classes, kpc_config_t *config);
static int (*kpc_get_thread_counters)(u32 tid, u32 buf_count, u64 *buf);
static int (*kpc_set_counting)(u32 classes);
static int (*kpc_set_thread_counting)(u32 classes);

// kperf PET functions
static int (*kperf_action_count_set)(u32 count);
static int (*kperf_action_count_get)(u32 *count);
static int (*kperf_action_samplers_set)(u32 actionid, u32 sample);
static int (*kperf_action_samplers_get)(u32 actionid, u32 *sample);
static int (*kperf_action_filter_set_by_pid)(u32 actionid, i32 pid);
static int (*kperf_timer_count_set)(u32 count);
static int (*kperf_timer_count_get)(u32 *count);
static int (*kperf_timer_period_set)(u32 actionid, u64 tick);
static int (*kperf_timer_period_get)(u32 actionid, u64 *tick);
static int (*kperf_timer_action_set)(u32 actionid, u32 timerid);
static int (*kperf_timer_action_get)(u32 actionid, u32 *timerid);
static int (*kperf_timer_pet_set)(u32 timerid);
static int (*kperf_timer_pet_get)(u32 *timerid);
static int (*kperf_sample_set)(u32 enabled);
static int (*kperf_sample_get)(u32 *enabled);
static int (*kperf_reset)(void);
static u64 (*kperf_ns_to_ticks)(u64 ns);
static u64 (*kperf_ticks_to_ns)(u64 ticks);
static u64 (*kperf_tick_frequency)(void);

// kperfdata.framework types
typedef struct kpep_db kpep_db;
typedef struct kpep_config kpep_config;

typedef struct kpep_event {
    const char *name;
    const char *description;
    const char *errata;
    const char *alias;
    const char *fallback;
    u32 mask;
    uint8_t number;
    uint8_t umask;
    uint8_t reserved;
    uint8_t is_fixed;
} kpep_event;

// Expose db internals we need
typedef struct kpep_db_internal {
    const char *name;
    const char *cpu_id;
    const char *marketing_name;
    void *plist_data;
    void *event_map;
    kpep_event *event_arr;
    kpep_event **fixed_event_arr;
    void *alias_map;
    usize reserved_1;
    usize reserved_2;
    usize reserved_3;
    usize event_count;
    usize alias_count;
    usize fixed_counter_count;
    usize config_counter_count;
    usize power_counter_count;
    u32 architecture;
    u32 fixed_counter_bits;
    u32 config_counter_bits;
    u32 power_counter_bits;
} kpep_db_internal;

// kperfdata.framework functions
static int (*kpep_db_create)(const char *name, kpep_db **db_ptr);
static void (*kpep_db_free)(kpep_db *db);
static int (*kpep_db_name)(kpep_db *db, const char **name);
static int (*kpep_db_event)(kpep_db *db, const char *name, kpep_event **ev_ptr);
static int (*kpep_db_events_count)(kpep_db *db, usize *count);
static int (*kpep_db_events)(kpep_db *db, kpep_event **buf, usize buf_size);
static int (*kpep_config_create)(kpep_db *db, kpep_config **cfg_ptr);
static void (*kpep_config_free)(kpep_config *cfg);
static int (*kpep_config_add_event)(kpep_config *cfg, kpep_event **ev_ptr,
                                     u32 flag, u32 *err);
static int (*kpep_config_force_counters)(kpep_config *cfg);
static int (*kpep_config_kpc_classes)(kpep_config *cfg, u32 *classes_ptr);
static int (*kpep_config_kpc_count)(kpep_config *cfg, usize *count_ptr);
static int (*kpep_config_kpc_map)(kpep_config *cfg, usize *buf, usize buf_size);
static int (*kpep_config_kpc)(kpep_config *cfg, kpc_config_t *buf, usize buf_size);

// ============================================================================
// kdebug structures and constants
// ============================================================================

// On both arm64 and x86_64 macOS (LP64), kd_buf args are 64-bit.
// The arm64 headers use uint64_t; x86_64 headers use uintptr_t (same size).
#if defined(__arm64__)
typedef uint64_t kd_buf_argtype;
#else
typedef uintptr_t kd_buf_argtype;
#endif

typedef struct {
    uint64_t timestamp;
    kd_buf_argtype arg1;
    kd_buf_argtype arg2;
    kd_buf_argtype arg3;
    kd_buf_argtype arg4;
    kd_buf_argtype arg5;  // thread ID
    uint32_t debugid;
#if defined(__LP64__) || defined(__arm64__)
    uint32_t cpuid;
    kd_buf_argtype unused;
#endif
} kd_buf;

typedef struct {
    unsigned int type;
    unsigned int value1;
    unsigned int value2;
    unsigned int value3;
    unsigned int value4;
} kd_regtype;

#define KDBG_CLASSTYPE   0x10000
#define KDBG_SUBCLSTYPE  0x20000
#define KDBG_RANGETYPE   0x40000
#define KDBG_TYPENONE    0x80000
#define KDBG_VALCHECK    0x00200000U

// kdebug class/subclass for kperf PMC data
/* #define DBG_PERF              6 */
#define PERF_KPC              6
#define PERF_KPC_DATA_THREAD  8

// task-clock and page-faults come from the same trace. A context switch is
// logged by the outgoing thread (arg5) and names the incoming one (arg2);
// stack_handoff() switches without a MACH_SCHED record, so it is needed too.
// A fault is logged by the faulting thread.
#define SCHED_SWITCH_EVENT    MACHDBG_CODE(DBG_MACH_SCHED, MACH_SCHED)
#define SCHED_HANDOFF_EVENT   MACHDBG_CODE(DBG_MACH_SCHED, MACH_STACK_HANDOFF)
#define VM_FAULT_EVENT        MACHDBG_CODE(DBG_MACH_VM, 2)  // vm_fault()

// KERN_KDGETBUF's reply (kbufinfo_t in XNU)
typedef struct {
    int nkdbufs;
    int nolog;
    unsigned int flags;
    int nkdthreads;
    int bufid;
} kd_bufinfo_t;

#define KDBG_WRAPPED 0x0008

// ============================================================================
// Framework Loading
// ============================================================================

#define KPERF_PATH     "/System/Library/PrivateFrameworks/kperf.framework/kperf"
#define KPERFDATA_PATH "/System/Library/PrivateFrameworks/kperfdata.framework/kperfdata"

static void *lib_kperf = NULL;
static void *lib_kperfdata = NULL;

static bool load_frameworks(void) {
    lib_kperf = dlopen(KPERF_PATH, RTLD_LAZY);
    if (!lib_kperf) {
        fprintf(stderr, "Failed to load kperf.framework: %s\n", dlerror());
        return false;
    }

    lib_kperfdata = dlopen(KPERFDATA_PATH, RTLD_LAZY);
    if (!lib_kperfdata) {
        fprintf(stderr, "Failed to load kperfdata.framework: %s\n", dlerror());
        return false;
    }

    #define LOAD_SYM(lib, name) do { \
        name = dlsym(lib, #name); \
        if (!name) { \
            fprintf(stderr, "Failed to load symbol: %s\n", #name); \
            return false; \
        } \
    } while(0)

    // kperf symbols
    LOAD_SYM(lib_kperf, kpc_force_all_ctrs_get);
    LOAD_SYM(lib_kperf, kpc_force_all_ctrs_set);
    LOAD_SYM(lib_kperf, kpc_get_counter_count);
    LOAD_SYM(lib_kperf, kpc_get_config_count);
    LOAD_SYM(lib_kperf, kpc_set_config);
    LOAD_SYM(lib_kperf, kpc_get_thread_counters);
    LOAD_SYM(lib_kperf, kpc_set_counting);
    LOAD_SYM(lib_kperf, kpc_set_thread_counting);

    // kperf PET symbols
    LOAD_SYM(lib_kperf, kperf_action_count_set);
    LOAD_SYM(lib_kperf, kperf_action_count_get);
    LOAD_SYM(lib_kperf, kperf_action_samplers_set);
    LOAD_SYM(lib_kperf, kperf_action_samplers_get);
    LOAD_SYM(lib_kperf, kperf_action_filter_set_by_pid);
    LOAD_SYM(lib_kperf, kperf_timer_count_set);
    LOAD_SYM(lib_kperf, kperf_timer_count_get);
    LOAD_SYM(lib_kperf, kperf_timer_period_set);
    LOAD_SYM(lib_kperf, kperf_timer_period_get);
    LOAD_SYM(lib_kperf, kperf_timer_action_set);
    LOAD_SYM(lib_kperf, kperf_timer_action_get);
    LOAD_SYM(lib_kperf, kperf_timer_pet_set);
    LOAD_SYM(lib_kperf, kperf_timer_pet_get);
    LOAD_SYM(lib_kperf, kperf_sample_set);
    LOAD_SYM(lib_kperf, kperf_sample_get);
    LOAD_SYM(lib_kperf, kperf_reset);
    LOAD_SYM(lib_kperf, kperf_ns_to_ticks);
    LOAD_SYM(lib_kperf, kperf_ticks_to_ns);
    LOAD_SYM(lib_kperf, kperf_tick_frequency);

    // kperfdata symbols
    LOAD_SYM(lib_kperfdata, kpep_db_create);
    LOAD_SYM(lib_kperfdata, kpep_db_free);
    LOAD_SYM(lib_kperfdata, kpep_db_name);
    LOAD_SYM(lib_kperfdata, kpep_db_event);
    LOAD_SYM(lib_kperfdata, kpep_db_events_count);
    LOAD_SYM(lib_kperfdata, kpep_db_events);
    LOAD_SYM(lib_kperfdata, kpep_config_create);
    LOAD_SYM(lib_kperfdata, kpep_config_free);
    LOAD_SYM(lib_kperfdata, kpep_config_add_event);
    LOAD_SYM(lib_kperfdata, kpep_config_force_counters);
    LOAD_SYM(lib_kperfdata, kpep_config_kpc_classes);
    LOAD_SYM(lib_kperfdata, kpep_config_kpc_count);
    LOAD_SYM(lib_kperfdata, kpep_config_kpc_map);
    LOAD_SYM(lib_kperfdata, kpep_config_kpc);

    #undef LOAD_SYM
    return true;
}

// ============================================================================
// kdebug utilities
// ============================================================================

static int kdebug_reset(void) {
    int mib[3] = { CTL_KERN, KERN_KDEBUG, KERN_KDREMOVE };
    return sysctl(mib, 3, NULL, NULL, NULL, 0);
}

static int kdebug_reinit(void) {
    int mib[3] = { CTL_KERN, KERN_KDEBUG, KERN_KDSETUP };
    return sysctl(mib, 3, NULL, NULL, NULL, 0);
}

static int kdebug_setreg(kd_regtype *kdr) {
    int mib[3] = { CTL_KERN, KERN_KDEBUG, KERN_KDSETREG };
    usize size = sizeof(kd_regtype);
    return sysctl(mib, 3, kdr, &size, NULL, 0);
}

static int kdebug_trace_setbuf(int nbufs) {
    int mib[4] = { CTL_KERN, KERN_KDEBUG, KERN_KDSETBUF, nbufs };
    return sysctl(mib, 4, NULL, NULL, NULL, 0);
}

static int kdebug_trace_enable(bool enable) {
    int mib[4] = { CTL_KERN, KERN_KDEBUG, KERN_KDENABLE, enable };
    return sysctl(mib, 4, NULL, 0, NULL, 0);
}

static int kdebug_trace_read(void *buf, usize len, usize *count) {
    if (count) *count = 0;
    if (!buf || !len) return -1;
    int mib[3] = { CTL_KERN, KERN_KDEBUG, KERN_KDREADTR };
    int ret = sysctl(mib, 3, buf, &len, NULL, 0);
    if (ret != 0) return ret;
    *count = len;
    return 0;
}

// The kernel overwrote events we had not read yet.
static bool kdebug_wrapped(void) {
    kd_bufinfo_t info = { 0 };
    usize size = sizeof(info);
    int mib[3] = { CTL_KERN, KERN_KDEBUG, KERN_KDGETBUF };
    return sysctl(mib, 3, &info, &size, NULL, 0) == 0 && (info.flags & KDBG_WRAPPED);
}

// Lightweight PET mode (not in framework, use sysctl directly)
static int kperf_lightweight_pet_set(u32 enabled) {
    return sysctlbyname("kperf.lightweight_pet", NULL, NULL, &enabled, 4);
}

// ============================================================================
// Event Aliases
// ============================================================================

typedef struct {
    const char *alias;
    const char *names[8];
} event_alias_t;

static const event_alias_t builtin_aliases[] = {
    // Core counters (Apple Silicon names first, then Intel)
    { "cycles", { "FIXED_CYCLES", "CPU_CLK_UNHALTED.THREAD", "CPU_CLK_UNHALTED.CORE", NULL }},
    { "instructions", { "FIXED_INSTRUCTIONS", "INST_RETIRED.ANY", NULL }},
    { "branches", { "INST_BRANCH", "BR_INST_RETIRED.ALL_BRANCHES", NULL }},
    { "branch-misses", { "BRANCH_MISPRED_NONSPEC", "BRANCH_MISPREDICT", "BR_MISP_RETIRED.ALL_BRANCHES", NULL }},

    // TLB misses
    { "l1d-tlb-misses", { "L1D_TLB_MISS", "L1D_TLB_MISS_NONSPEC", "DTLB_LOAD_MISSES.MISS_CAUSES_A_WALK", NULL }},
    { "l1i-tlb-misses", { "L1I_TLB_MISS_DEMAND", "ITLB_MISSES.MISS_CAUSES_A_WALK", NULL }},
    { "l2-tlb-misses-data", { "L2_TLB_MISS_DATA", "DTLB_LOAD_MISSES.WALK_COMPLETED", NULL }},

    // Cache misses
    { "l1d-cache-misses", { "L1D_CACHE_MISS_LD", "L1D_CACHE_MISS_LD_NONSPEC", "MEM_LOAD_RETIRED.L1_MISS", "L1D.REPLACEMENT", NULL }},
    { "l1i-cache-misses", { "L1I_CACHE_MISS_DEMAND", "ICACHE.MISSES", NULL }},
    { "llc-misses", { "L2_CACHE_MISS_DATA", "LONGEST_LAT_CACHE.MISS", "MEM_LOAD_RETIRED.L3_MISS", NULL }},

    // Intel reference cycles (fixed counter, no Apple Silicon equivalent)
    { "ref-cycles", { "CPU_CLK_UNHALTED.REF_TSC", NULL }},

    // Apple Silicon specific (no Intel equivalents)
    { "map-stalls", { "MAP_STALL", NULL }},
    { "dispatch-stalls", { "MAP_STALL_DISPATCH", NULL }},
    { NULL, { NULL } }
};

static kpep_event *find_event(kpep_db *db, const char *name) {
    kpep_event *ev = NULL;
    if (kpep_db_event(db, name, &ev) == 0 && ev) return ev;
    for (const event_alias_t *alias = builtin_aliases; alias->alias; alias++) {
        if (strcasecmp(name, alias->alias) == 0) {
            for (int i = 0; alias->names[i]; i++) {
                if (kpep_db_event(db, alias->names[i], &ev) == 0 && ev) return ev;
            }
        }
    }
    return NULL;
}

// ============================================================================
// PMC State
// ============================================================================

#define MAX_EVENTS 10   // hardware counters
#define MAX_LISTED 16   // everything -e may name, software and tool events too

// perf's software events that mperf derives from the kernel trace, and its
// tool events, which are just the run's own times. Anything else is a PMC.
typedef enum {
    EVENT_PMC,
    EVENT_TASK_CLOCK,     // ns on CPU, reported in msec
    EVENT_PAGE_FAULTS,
    EVENT_DURATION_TIME,  // the rest are ns
    EVENT_USER_TIME,
    EVENT_SYSTEM_TIME
} event_kind_t;

static const struct { const char *name; event_kind_t kind; } named_events[] = {
    { "task-clock",    EVENT_TASK_CLOCK },
    { "page-faults",   EVENT_PAGE_FAULTS },
    { "faults",        EVENT_PAGE_FAULTS },  // perf's alias
    { "duration_time", EVENT_DURATION_TIME },
    { "user_time",     EVENT_USER_TIME },
    { "system_time",   EVENT_SYSTEM_TIME },
    { NULL, EVENT_PMC }
};

static event_kind_t event_kind(const char *name) {
    for (int i = 0; named_events[i].name; i++)
        if (strcmp(name, named_events[i].name) == 0) return named_events[i].kind;
    return EVENT_PMC;
}

typedef struct {
    const char *name;
    event_kind_t kind;
    kpep_event *event;   // EVENT_PMC only
    int pmc_index;       // EVENT_PMC only: position in the kpep config
    u64 value;
} configured_event_t;

typedef struct {
    kpep_db *db;
    kpep_config *config;
    u32 classes;
    u32 counter_count;
    usize reg_count;
    kpc_config_t regs[KPC_MAX_COUNTERS];
    usize counter_map[KPC_MAX_COUNTERS];

    configured_event_t events[MAX_LISTED];
    int event_count;
    int pmc_count;
    bool count_faults;   // page-faults was asked for

    double wall_time_ns;
    double user_time_ns;
    double sys_time_ns;
    u64 task_clock_ns;   // always measured: it is every counter's event-runtime
    u64 page_faults;

    // -p target; 0 when mperf launched the command itself
    pid_t attached_pid;
} pmc_state_t;

static bool pmc_init(pmc_state_t *state) {
    memset(state, 0, sizeof(*state));

    int force_ctrs = 0;
    if (kpc_force_all_ctrs_get(&force_ctrs)) {
        fprintf(stderr, "Error: Root privileges required (run with sudo)\n");
        return false;
    }

    if (kpep_db_create(NULL, &state->db) != 0) {
        fprintf(stderr, "Error: Failed to load PMC database\n");
        return false;
    }

    if (kpep_config_create(state->db, &state->config) != 0) {
        fprintf(stderr, "Error: Failed to create PMC config\n");
        return false;
    }

    if (kpep_config_force_counters(state->config) != 0) {
        fprintf(stderr, "Error: Failed to force counters\n");
        return false;
    }

    return true;
}

// Adds a hardware counter to the kpep config; `listed` says whether it is
// reported or only there so that PET has something to sample.
static bool pmc_add_counter(pmc_state_t *state, const char *name, bool listed) {
    if (state->pmc_count >= MAX_EVENTS) {
        fprintf(stderr, "Error: Too many hardware events (max %d)\n", MAX_EVENTS);
        return false;
    }

    kpep_event *ev = find_event(state->db, name);
    if (!ev) {
        fprintf(stderr, "Error: Unknown event '%s'\n", name);
        return false;
    }

    u32 err = 0;
    if (kpep_config_add_event(state->config, &ev, 0, &err) != 0) {
        kpep_db_internal *db_int = (kpep_db_internal *)state->db;
        fprintf(stderr, "Error: Failed to add event '%s' (conflict: 0x%x)\n", name, err);
        fprintf(stderr, "  This CPU has %zu fixed + %zu configurable counters\n",
                db_int->fixed_counter_count, db_int->config_counter_count);
        return false;
    }

    if (listed) {
        configured_event_t *e = &state->events[state->event_count++];
        e->name = name;
        e->kind = EVENT_PMC;
        e->event = ev;
        e->pmc_index = state->pmc_count;
    }
    state->pmc_count++;
    return true;
}

static bool pmc_add_event(pmc_state_t *state, const char *name) {
    if (state->event_count >= MAX_LISTED) {
        fprintf(stderr, "Error: Too many events (max %d)\n", MAX_LISTED);
        return false;
    }

    event_kind_t kind = event_kind(name);
    if (kind == EVENT_PMC) return pmc_add_counter(state, name, true);

    configured_event_t *e = &state->events[state->event_count++];
    e->name = name;
    e->kind = kind;
    if (kind == EVENT_PAGE_FAULTS) state->count_faults = true;
    return true;
}

static bool pmc_configure(pmc_state_t *state) {
    if (kpep_config_kpc_classes(state->config, &state->classes) != 0 ||
        kpep_config_kpc_count(state->config, &state->reg_count) != 0 ||
        kpep_config_kpc_map(state->config, state->counter_map, sizeof(state->counter_map)) != 0 ||
        kpep_config_kpc(state->config, state->regs, sizeof(state->regs)) != 0) {
        fprintf(stderr, "Error: Failed to get PMC configuration\n");
        return false;
    }

    if (kpc_force_all_ctrs_set(1) != 0) {
        fprintf(stderr, "Error: Failed to force counters\n");
        return false;
    }

    if ((state->classes & KPC_CLASS_CONFIGURABLE_MASK) && state->reg_count) {
        if (kpc_set_config(state->classes, state->regs) != 0) {
            fprintf(stderr, "Error: Failed to set PMC config\n");
            return false;
        }
    }

    state->counter_count = kpc_get_counter_count(state->classes);
    return true;
}

static void pmc_cleanup(pmc_state_t *state) {
    if (state->config) kpep_config_free(state->config);
    if (state->db) kpep_db_free(state->db);
}

// ============================================================================
// Thread tracking for PET aggregation
// ============================================================================

#define MAX_THREADS 256

typedef struct {
    u32 tid;
    u64 timestamp_first;
    u64 timestamp_last;
    u64 counters_first[KPC_MAX_COUNTERS];
    u64 counters_last[KPC_MAX_COUNTERS];
    bool has_first;
    bool has_last;
} thread_data_t;

typedef struct {
    thread_data_t threads[MAX_THREADS];
    int count;
} thread_tracker_t;

static thread_data_t *tracker_get_or_create(thread_tracker_t *tracker, u32 tid) {
    for (int i = 0; i < tracker->count; i++) {
        if (tracker->threads[i].tid == tid) {
            return &tracker->threads[i];
        }
    }
    if (tracker->count >= MAX_THREADS) {
        fprintf(stderr, "Warning: Too many threads, some data may be lost\n");
        return NULL;
    }
    thread_data_t *t = &tracker->threads[tracker->count++];
    memset(t, 0, sizeof(*t));
    t->tid = tid;
    return t;
}

// ============================================================================
// task-clock and page-faults from the scheduler and VM trace
// ============================================================================

// The kdebug pid check cannot narrow these events to the target: exec gives the
// process a new proc struct without the flag, and scheduler events ignore it
// anyway. So time on CPU and faults are tallied per thread for the whole
// system, then summed over the threads PET sampled, which kperf's PID filter
// makes exactly the target's. A thread that lived less than one PET period
// and was never sampled is missed.
#define TRACE_MAX_CPUS  512
#define TID_SLOTS       (1u << 17)

typedef struct {
    u64 tid;      // 0: empty slot
    u64 on_cpu;   // mach absolute time units
    u64 faults;
    bool target;
} tid_usage_t;

typedef struct {
    tid_usage_t *slots;
    u64 cpu_tid[TRACE_MAX_CPUS];    // thread switched in last, per CPU
    u64 cpu_since[TRACE_MAX_CPUS];  // and when; 0 until the first switch
    bool full;
} usage_tracker_t;

static tid_usage_t *usage_get(usage_tracker_t *u, u64 tid) {
    u64 h = (tid * 0x9E3779B97F4A7C15ull) & (TID_SLOTS - 1);
    for (u64 i = 0; i < TID_SLOTS; i++) {
        tid_usage_t *t = &u->slots[(h + i) & (TID_SLOTS - 1)];
        if (t->tid == tid) return t;
        if (t->tid == 0) {
            t->tid = tid;
            return t;
        }
    }
    u->full = true;
    return NULL;
}

static void usage_event(usage_tracker_t *u, const kd_buf *b) {
    u32 id = b->debugid & KDBG_EVENTID_MASK;
    tid_usage_t *t;

    if (id == SCHED_SWITCH_EVENT || id == SCHED_HANDOFF_EVENT) {
        u32 cpu = b->cpuid;
        if (cpu >= TRACE_MAX_CPUS) return;
        if (u->cpu_since[cpu] && u->cpu_tid[cpu] == b->arg5 &&
            (t = usage_get(u, b->arg5)))
            t->on_cpu += b->timestamp - u->cpu_since[cpu];
        u->cpu_tid[cpu] = b->arg2;
        u->cpu_since[cpu] = b->timestamp;
    } else if (id == VM_FAULT_EVENT) {
        if ((b->debugid & KDBG_FUNC_MASK) != DBG_FUNC_END && (t = usage_get(u, b->arg5)))
            t->faults++;
    } else if (id == KDBG_EVENTID(DBG_PERF, PERF_KPC, PERF_KPC_DATA_THREAD)) {
        if ((t = usage_get(u, b->arg5))) t->target = true;
    }
}

// Threads the target already has when counting starts are its own even if PET
// never samples them. The one that matters is a launched command's thread
// before exec: exec does its work, faults included, in that thread and then
// carries on in a new one.
#define PROC_PIDLISTTHREADIDS 28  // XNU's proc_info_private.h

static void usage_mark_threads(usage_tracker_t *u, pid_t pid) {
    u64 tids[1024];
    int n = proc_pidinfo(pid, PROC_PIDLISTTHREADIDS, 0, tids, sizeof(tids));
    for (int i = 0; i < n / (int)sizeof(u64); i++) {
        tid_usage_t *t = usage_get(u, tids[i]);
        if (t) t->target = true;
    }
}

static void usage_totals(const usage_tracker_t *u, u64 *task_clock_ns, u64 *faults) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    u64 ticks = 0;
    *faults = 0;
    for (u64 i = 0; i < TID_SLOTS; i++) {
        if (!u->slots[i].target) continue;
        ticks += u->slots[i].on_cpu;
        *faults += u->slots[i].faults;
    }
    *task_clock_ns = (u64)((double)ticks * tb.numer / tb.denom);
}

// ============================================================================
// Signal handling for clean shutdown
// ============================================================================

static volatile sig_atomic_t g_interrupted = 0;

static void sigint_handler(int sig) {
    (void)sig;
    g_interrupted = 1;
}

// Tear down kperf/kdebug kernel state
static void profiling_cleanup(void) {
    kdebug_trace_enable(0);
    kdebug_reset();
    kperf_sample_set(0);
    kperf_lightweight_pet_set(0);
    kpc_set_counting(0);
    kpc_set_thread_counting(0);
    kpc_force_all_ctrs_set(0);
}

// How the measured command finished. `reaped` says whether `status` holds a
// real wait(2) status; it is false when the tool failed before the command
// could be waited for, and the caller then reports a tool error instead.
typedef struct {
    bool counted;  // counting started, so there is a report to write
    bool reaped;
    int status;
} child_outcome_t;

// ============================================================================
// Privileges
// ============================================================================

// The user who mperf runs on behalf of. Programming the PMU needs root: the 
// measured command, and every file opened by name, belong to the user who 
// invoked mperf, whether root came from sudo or from a setuid install.
typedef struct {
    bool drop;         // false when root itself ran mperf, without sudo
    bool setuid;       // setuid install: the real uid is the invoking user
    uid_t uid;
    gid_t gid;
    const char *name;  // sudo only, for initgroups(); NULL means no lookup
} invoker_t;

static invoker_t g_invoker;

static bool parse_id(const char *s, unsigned long *out) {
    char *end;
    errno = 0;
    *out = strtoul(s, &end, 10);
    return *s && !*end && !errno;
}

static bool invoker_init(void) {
    if (getuid() != geteuid()) {
        g_invoker = (invoker_t){ .drop = true, .setuid = true,
                                 .uid = getuid(), .gid = getgid() };
        return true;
    }

    // Only trust SUDO_* when the real uid is root. Under a setuid install the
    // caller controls the environment, and that case was settled above.
    const char *su = getenv("SUDO_UID"), *sg = getenv("SUDO_GID");
    if (getuid() != 0 || !su || !sg) return true;

    unsigned long uid, gid;
    if (!parse_id(su, &uid) || !parse_id(sg, &gid)) {
        fprintf(stderr, "Error: malformed SUDO_UID/SUDO_GID\n");
        return false;
    }
    struct passwd *pw = getpwuid((uid_t)uid);
    g_invoker = (invoker_t){ .drop = uid != 0, .uid = (uid_t)uid, .gid = (gid_t)gid,
                             .name = pw ? strdup(pw->pw_name) : NULL };
    return true;
}

// Permanently become the invoking user; for the child, just before exec.
static bool drop_privileges(void) {
    if (!g_invoker.drop) return true;

    if (!g_invoker.setuid) {
        int rc = g_invoker.name ? initgroups(g_invoker.name, (int)g_invoker.gid)
                                : setgroups(1, &g_invoker.gid);
        if (rc != 0) return false;
    }
    if (setgid(g_invoker.gid) != 0 || setuid(g_invoker.uid) != 0) return false;
    // setuid() as root sets the saved uid too; make sure root is really gone.
    return setuid(0) != 0 && getuid() == g_invoker.uid && geteuid() == g_invoker.uid;
}

// open(2) a path the invoking user named, with the user's permissions, so a
// setuid mperf cannot be used to create or truncate files the user could not.
// Only the effective ids change, so root comes back afterwards. Under sudo the
// supplementary groups stay root's, which only matters to someone who is root.
static int open_as_invoker(const char *path, int flags, mode_t mode) {
    if (!g_invoker.drop) return open(path, flags, mode);

    gid_t egid = getegid();
    if (setegid(g_invoker.gid) != 0) return -1;
    if (seteuid(g_invoker.uid) != 0) {
        int err = errno;
        setegid(egid);
        errno = err;
        return -1;
    }
    int fd = open(path, flags, mode);
    int err = errno;
    if (seteuid(0) != 0 || setegid(egid) != 0) {
        // Without root back, teardown of the kernel state would fail later.
        perror("Error: cannot regain root after opening a file");
        exit(1);
    }
    errno = err;
    return fd;
}

// -p is refused for processes the invoking user could not ptrace themselves,
// mirroring perf's same-uid rule. Root, including via sudo, may attach anywhere.
static bool may_attach(pid_t pid) {
    struct proc_bsdinfo bi;
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof(bi)) != (int)sizeof(bi)) {
        fprintf(stderr, "Error: no such process: %d\n", pid);
        return false;
    }
    if (!g_invoker.setuid) return true;
    if (bi.pbi_uid != g_invoker.uid || bi.pbi_ruid != g_invoker.uid ||
        bi.pbi_svuid != g_invoker.uid || (bi.pbi_flags & PROC_FLAG_PSUGID)) {
        fprintf(stderr, "Error: process %d does not belong to you\n", pid);
        return false;
    }
    return true;
}

// ============================================================================
// Control protocol (perf stat --delay -1 --control)
// ============================================================================

// Implements some of perf's --control protocol for an armed start: block for "enable",
// set up, then answer "ack\n" once counting. A harness that already drives
// `perf stat --delay -1 --control fifo:ctl,ack` drives mperf the same way.
// "ping" is acknowledged as perf does and anything else is reported and ignored.
//
// Unlike perf, a command mperf launches is held until counting starts rather
// than started straight away with events disabled: threads it creates before
// PET is armed would never be counted.
typedef struct {
    int ctl_fd;   // -1 when there is no --control
    int ack_fd;   // -1 when --control named no ack channel
} control_t;

static bool parse_control(const char *spec, control_t *c) {
    c->ctl_fd = c->ack_fd = -1;
    char *copy = strdup(spec);
    if (!copy) return false;
    char *ack = strchr(copy, ',');
    if (ack) *ack++ = '\0';

    bool ok = false;
    if (strncmp(copy, "fifo:", 5) == 0) {
        // O_RDWR, as perf does: never blocks, and the fifo never reports EOF.
        c->ctl_fd = open_as_invoker(copy + 5, O_RDWR | O_CLOEXEC, 0);
        c->ack_fd = ack ? open_as_invoker(ack, O_WRONLY | O_CLOEXEC, 0) : -1;
        ok = c->ctl_fd >= 0 && (!ack || c->ack_fd >= 0);
        if (!ok) fprintf(stderr, "Error: cannot open --control fifo: %s\n", strerror(errno));
    } else if (strncmp(copy, "fd:", 3) == 0) {
        unsigned long ctl, ackfd = 0;
        ok = parse_id(copy + 3, &ctl) && (!ack || parse_id(ack, &ackfd));
        c->ctl_fd = (int)ctl;
        c->ack_fd = ack ? (int)ackfd : -1;
        if (!ok) fprintf(stderr, "Error: bad --control fd spec: %s\n", spec);
    } else {
        fprintf(stderr, "Error: --control takes fifo:CTL[,ACK] or fd:CTL[,ACK]\n");
    }
    free(copy);
    return ok;
}

static void control_ack(const control_t *c) {
    if (c->ack_fd >= 0 && write(c->ack_fd, "ack\n", 4) != 4)
        perror("Warning: cannot write control ack");
}

static bool control_wait_enable(const control_t *c) {
    char buf[128];
    usize len = 0;
    for (;;) {
        ssize_t n = read(c->ctl_fd, buf + len, sizeof(buf) - 1 - len);
        if (n < 0 && errno == EINTR && !g_interrupted) continue;
        if (n <= 0) {
            fprintf(stderr, "Error: control channel closed before \"enable\"\n");
            return false;
        }
        len += (usize)n;
        buf[len] = '\0';

        char *nl;
        while ((nl = memchr(buf, '\n', len))) {
            *nl = '\0';
            if (strcmp(buf, "enable") == 0) return true;
            if (strcmp(buf, "ping") == 0) control_ack(c);
            else fprintf(stderr, "Warning: ignoring control command \"%s\"\n", buf);
            len -= (usize)(nl + 1 - buf);
            memmove(buf, nl + 1, len);
            buf[len] = '\0';
        }
        if (len == sizeof(buf) - 1) len = 0;  // an overlong line: drop it
    }
}

// ============================================================================
// PET-based measurement (accurate, multi-threaded)
// ============================================================================

// `exit_kq` is set in attach mode (-p): the target is not mperf's child, so its
// exit is watched through kqueue, which also fires while it is still an
// unreaped zombie of whoever does own it.
static bool run_with_pet(pmc_state_t *state, pid_t target_pid,
                         double sample_period_sec, bool verbose, int sync_fd,
                         int exit_kq, const control_t *control,
                         child_outcome_t *outcome) {
    bool success = false;
    int ret;

    if (control->ctl_fd >= 0 && !control_wait_enable(control))
        goto fail_early;

    // Start counting
    if (kpc_set_counting(state->classes) != 0 ||
        kpc_set_thread_counting(state->classes) != 0) {
        fprintf(stderr, "Error: Failed to start counting\n");
        goto fail_early;
    }

    // CRITICAL: Allow counters to initialize (fixes intermittent zeros)
    // See: https://gist.github.com/ibireme/173517c208c7dc333ba962c1f0d67d12
    usleep(1000);

    // Set up kperf actions and timers
    u32 actionid = 1;
    u32 timerid = 1;

    // A killed mperf leaves sampling on, and kperf cannot be reconfigured
    // while it is. perf has no such problem: the kernel drops its events.
    kperf_sample_set(0);
    kperf_lightweight_pet_set(0);

    if ((ret = kperf_action_count_set(KPERF_ACTION_MAX))) {
        fprintf(stderr, "Error: Failed to set action count: %d\n", ret);
        goto fail_early;
    }
    if ((ret = kperf_timer_count_set(KPERF_TIMER_MAX))) {
        fprintf(stderr, "Error: Failed to set timer count: %d\n", ret);
        goto fail_early;
    }
    if ((ret = kperf_action_samplers_set(actionid, KPERF_SAMPLER_PMC_THREAD))) {
        fprintf(stderr, "Error: Failed to set sampler type: %d\n", ret);
        goto fail_early;
    }
    if ((ret = kperf_action_filter_set_by_pid(actionid, target_pid))) {
        fprintf(stderr, "Error: Failed to set PID filter: %d\n", ret);
        goto fail_early;
    }

    // Set up timer for PET
    u64 tick = kperf_ns_to_ticks((u64)(sample_period_sec * 1e9));
    if ((ret = kperf_timer_period_set(actionid, tick))) {
        fprintf(stderr, "Error: Failed to set timer period: %d\n", ret);
        goto fail_early;
    }
    if ((ret = kperf_timer_action_set(actionid, timerid))) {
        fprintf(stderr, "Error: Failed to set timer action: %d\n", ret);
        goto fail_early;
    }
    if ((ret = kperf_timer_pet_set(timerid))) {
        fprintf(stderr, "Error: Failed to set timer PET: %d\n", ret);
        goto fail_early;
    }
    if ((ret = kperf_lightweight_pet_set(1))) {
        fprintf(stderr, "Error: Failed to set lightweight PET: %d\n", ret);
        goto fail_early;
    }
    if ((ret = kperf_sample_set(1))) {
        fprintf(stderr, "Error: Failed to start sampling: %d\n", ret);
        goto fail_early;
    }

    // Set up kdebug tracing
    if ((ret = kdebug_reset())) {
        fprintf(stderr, "Error: Failed to reset kdebug: %d\n", ret);
        goto fail_early;
    }

    int nbufs = 1000000;
    if ((ret = kdebug_trace_setbuf(nbufs))) {
        fprintf(stderr, "Error: Failed to set kdebug buffer: %d\n", ret);
        goto fail_early;
    }
    if ((ret = kdebug_reinit())) {
        fprintf(stderr, "Error: Failed to init kdebug: %d\n", ret);
        goto fail_early;
    }

    // Keep exactly four event IDs, the most the kernel matches: PMC thread
    // data, the two kinds of context switch, and faults when they are counted
    // (otherwise PMC data again). Fault records dominate a fault-heavy program's
    // trace, which is why they are left out unless asked for.
    kd_regtype kdr = { 0 };
    kdr.type = KDBG_VALCHECK;
    kdr.value1 = KDBG_EVENTID(DBG_PERF, PERF_KPC, PERF_KPC_DATA_THREAD);
    kdr.value2 = SCHED_SWITCH_EVENT;
    kdr.value3 = SCHED_HANDOFF_EVENT;
    kdr.value4 = state->count_faults ? VM_FAULT_EVENT : kdr.value1;
    if ((ret = kdebug_setreg(&kdr))) {
        fprintf(stderr, "Error: Failed to set kdebug filter: %d\n", ret);
        goto fail_early;
    }
    usage_tracker_t usage = { .slots = calloc(TID_SLOTS, sizeof(tid_usage_t)) };
    if (!usage.slots) {
        fprintf(stderr, "Error: Failed to allocate thread table\n");
        goto fail_early;
    }
    if ((ret = kdebug_trace_enable(1))) {
        fprintf(stderr, "Error: Failed to enable kdebug: %d\n", ret);
        free(usage.slots);
        goto fail_early;
    }
    usage_mark_threads(&usage, target_pid);

    // Counting: the controller may release its target now.
    outcome->counted = true;
    control_ack(control);

    // NOW let the child process start executing
    // Closing the write end of the pipe unblocks the child's read()
    if (sync_fd >= 0) {
        close(sync_fd);
        sync_fd = -1;
    }

    // Allocate trace buffer
    usize buf_capacity = nbufs * 2;
    kd_buf *buf_storage = malloc(sizeof(kd_buf) * buf_capacity);
    kd_buf *buf_cur = buf_storage;
    kd_buf *buf_end = buf_storage + buf_capacity;

    if (!buf_storage) {
        fprintf(stderr, "Error: Failed to allocate trace buffer\n");
        free(usage.slots);
        goto cleanup;
    }

    usize total_raw_samples = 0;
    usize total_filtered_samples = 0;
    bool wrapped = false;

    // Record start time
    struct timeval wall_start, wall_end;
    struct rusage usage_start, usage_end;
    gettimeofday(&wall_start, NULL);
    getrusage(RUSAGE_CHILDREN, &usage_start);

    // Wait for child and collect samples
    int status = 0;
    while (1) {
        usleep((useconds_t)(sample_period_sec * 1e6 * 2));

        if (g_interrupted) break;

        // Check if child exited. An attached target's status belongs to its
        // own parent; perf stat -p reports 0 for it, and so do we.
        pid_t result;
        if (exit_kq >= 0) {
            struct kevent ev;
            struct timespec zero = { 0, 0 };
            int n = kevent(exit_kq, NULL, 0, &ev, 1, &zero);
            result = n > 0 ? target_pid : n;
        } else {
            result = waitpid(target_pid, &status, WNOHANG);
        }
        if (result == -1 && errno != EINTR) break;
        if (result == target_pid) {
            outcome->reaped = true;
            outcome->status = status;
            // Child exited, do one more read then stop
            usleep((useconds_t)(sample_period_sec * 1e6));
        }

        // Expand buffer if needed
        if (buf_end - buf_cur < nbufs) {
            usize new_capacity = buf_capacity * 2;
            kd_buf *new_buf = realloc(buf_storage, sizeof(kd_buf) * new_capacity);
            if (!new_buf) {
                fprintf(stderr, "Error: Failed to expand buffer\n");
                break;
            }
            buf_cur = new_buf + (buf_cur - buf_storage);
            buf_end = new_buf + new_capacity;
            buf_storage = new_buf;
            buf_capacity = new_capacity;
        }

        // Read trace data. A read clears the wrapped flag, so check it first.
        usize count = 0;
        wrapped |= kdebug_wrapped();
        kdebug_trace_read(buf_cur, sizeof(kd_buf) * nbufs, &count);
        total_raw_samples += count;

        // Tally the scheduler and VM events, and keep the PMC thread data
        kd_buf *write_ptr = buf_cur;
        for (kd_buf *buf = buf_cur, *end = buf_cur + count; buf < end; buf++) {
            usage_event(&usage, buf);
            u32 debugid = buf->debugid;
            u32 cls = KDBG_EXTRACT_CLASS(debugid);
            u32 subcls = KDBG_EXTRACT_SUBCLASS(debugid);
            u32 code = KDBG_EXTRACT_CODE(debugid);

            if (cls == DBG_PERF && subcls == PERF_KPC && code == PERF_KPC_DATA_THREAD) {
                *write_ptr++ = *buf;
                total_filtered_samples++;
            }
        }
        buf_cur = write_ptr;

        if (result == target_pid) break;
    }

    // Record end time
    gettimeofday(&wall_end, NULL);
    getrusage(RUSAGE_CHILDREN, &usage_end);

    // Process collected samples
    thread_tracker_t tracker = { 0 };

    for (kd_buf *buf = buf_storage; buf < buf_cur; buf++) {
        u32 func = buf->debugid & KDBG_FUNC_MASK;
        if (func != DBG_FUNC_START) continue;

        u32 tid = (u32)buf->arg5;
        if (!tid) continue;

        // Read counter values (may span multiple kd_buf entries)
        u32 ci = 0;
        u64 counters[KPC_MAX_COUNTERS] = { 0 };
        counters[ci++] = buf->arg1;
        counters[ci++] = buf->arg2;
        counters[ci++] = buf->arg3;
        counters[ci++] = buf->arg4;

        if (ci < state->counter_count) {
            for (kd_buf *buf2 = buf + 1; buf2 < buf_cur && ci < state->counter_count; buf2++) {
                u32 tid2 = (u32)buf2->arg5;
                if (tid2 != tid) break;
                u32 func2 = buf2->debugid & KDBG_FUNC_MASK;
                if (func2 == DBG_FUNC_START) break;

                if (ci < state->counter_count) counters[ci++] = buf2->arg1;
                if (ci < state->counter_count) counters[ci++] = buf2->arg2;
                if (ci < state->counter_count) counters[ci++] = buf2->arg3;
                if (ci < state->counter_count) counters[ci++] = buf2->arg4;
            }
        }

        if (ci < state->counter_count) continue;  // Incomplete sample

        // Update thread tracker
        thread_data_t *td = tracker_get_or_create(&tracker, tid);
        if (!td) continue;

        if (!td->has_first) {
            td->timestamp_first = buf->timestamp;
            memcpy(td->counters_first, counters, sizeof(counters));
            td->has_first = true;
        }
        td->timestamp_last = buf->timestamp;
        memcpy(td->counters_last, counters, sizeof(counters));
        td->has_last = true;
    }

    free(buf_storage);

    usage_totals(&usage, &state->task_clock_ns, &state->page_faults);
    free(usage.slots);
    if (wrapped)
        fprintf(stderr, "Warning: the kernel trace buffer wrapped; counts are low\n");
    if (usage.full)
        fprintf(stderr, "Warning: too many threads in the system to track task-clock\n");

    // Aggregate across all threads
    u64 total_counters[KPC_MAX_COUNTERS] = { 0 };
    int threads_with_data = 0;

    for (int i = 0; i < tracker.count; i++) {
        thread_data_t *td = &tracker.threads[i];
        // Need at least 2 different samples (first != last timestamps)
        if (!td->has_first) continue;
        if (td->timestamp_last == td->timestamp_first) continue;  // Only one sample

        threads_with_data++;
        for (u32 c = 0; c < state->counter_count; c++) {
            total_counters[c] += td->counters_last[c] - td->counters_first[c];
        }
    }

    // Verbose diagnostic output
    if (verbose) {
        fprintf(stderr, "Raw kdebug samples: %zu\n", total_raw_samples);
        fprintf(stderr, "Filtered PMC samples: %zu\n", total_filtered_samples);
        fprintf(stderr, "Threads found: %d\n", tracker.count);
        fprintf(stderr, "Threads with valid data: %d\n", threads_with_data);
        fprintf(stderr, "Counter count: %u\n", state->counter_count);
        fprintf(stderr, "Counter map: ");
        for (int i = 0; i < state->pmc_count; i++) {
            fprintf(stderr, "%zu ", state->counter_map[i]);
        }
        fprintf(stderr, "\n");
    }

    if (threads_with_data == 0 && verbose) {
        fprintf(stderr, "Warning: No valid thread samples collected!\n");
        if (total_filtered_samples == 0) {
            fprintf(stderr, "  -> No PMC thread data in kdebug trace.\n");
            fprintf(stderr, "  -> Check if the target process ran too quickly.\n");
        }
    }

    // Map counter values to events
    for (int i = 0; i < state->event_count; i++) {
        if (state->events[i].kind != EVENT_PMC) continue;
        usize idx = state->counter_map[state->events[i].pmc_index];
        state->events[i].value = total_counters[idx];
    }

    // Compute times
    state->wall_time_ns =
        (wall_end.tv_sec - wall_start.tv_sec) * 1e9 +
        (wall_end.tv_usec - wall_start.tv_usec) * 1e3;

    struct timeval user_diff, sys_diff;
    timersub(&usage_end.ru_utime, &usage_start.ru_utime, &user_diff);
    timersub(&usage_end.ru_stime, &usage_start.ru_stime, &sys_diff);

    // RUSAGE_CHILDREN says nothing about a process we did not fork, so an
    // attached run has no user/sys time; perf stat -p prints no user/sys
    // lines either, and its user_time and system_time events read 0.
    if (exit_kq < 0) {
        state->user_time_ns = user_diff.tv_sec * 1e9 + user_diff.tv_usec * 1e3;
        state->sys_time_ns = sys_diff.tv_sec * 1e9 + sys_diff.tv_usec * 1e3;
    }

    for (int i = 0; i < state->event_count; i++) {
        configured_event_t *e = &state->events[i];
        switch (e->kind) {
            case EVENT_PMC:           break;
            case EVENT_TASK_CLOCK:    e->value = state->task_clock_ns; break;
            case EVENT_PAGE_FAULTS:   e->value = state->page_faults; break;
            case EVENT_DURATION_TIME: e->value = (u64)state->wall_time_ns; break;
            case EVENT_USER_TIME:     e->value = (u64)state->user_time_ns; break;
            case EVENT_SYSTEM_TIME:   e->value = (u64)state->sys_time_ns; break;
        }
    }

    // Measurement itself succeeded; whether the command did is the caller's
    // business, and is reported through the exit status rather than folded in.
    success = outcome->reaped;

cleanup:
    profiling_cleanup();
    return success;

fail_early:
    // As perf stat does when no event can be counted, the command never runs.
    if (sync_fd >= 0) {
        kill(target_pid, SIGKILL);
        close(sync_fd);
        waitpid(target_pid, NULL, 0);
    }
    profiling_cleanup();
    return false;
}

// SIGTERM is how a harness stops a tool; it gets the same cleanup as Ctrl+C,
// or the kernel would be left with PET sampling and the counters configured.
static void install_stop_handlers(struct sigaction old[2]) {
    struct sigaction sa = { .sa_handler = sigint_handler };
    sigaction(SIGINT, &sa, &old[0]);
    sigaction(SIGTERM, &sa, &old[1]);
}

static void restore_stop_handlers(const struct sigaction old[2]) {
    sigaction(SIGINT, &old[0], NULL);
    sigaction(SIGTERM, &old[1], NULL);
}

static bool run_command_pet(pmc_state_t *state, char **argv, double sample_period,
                            bool verbose, const control_t *control,
                            child_outcome_t *outcome) {
    outcome->reaped = false;
    outcome->status = 0;

    // Create a pipe for synchronization
    // Parent will set up profiling, then close the write end
    // Child will wait for read end to close before exec'ing
    int sync_pipe[2];
    if (pipe(sync_pipe) < 0) {
        perror("pipe");
        return false;
    }

    // Fork child
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        return false;
    }

    if (pid == 0) {
        // Child: because the PMU is processed via the parent, the command runs as the
        // invoking user rather than as root.
        if (!drop_privileges()) {
            perror("mperf: cannot drop privileges");
            _exit(126);
        }

        // Wait for parent to set up profiling
        close(sync_pipe[1]);  // Close write end

        // Block until parent closes its write end
        char dummy;
        (void)read(sync_pipe[0], &dummy, 1);
        close(sync_pipe[0]);

        // Now exec the command
        execvp(argv[0], argv);
        perror("execvp");
        _exit(127);
    }

    // Parent: set up profiling infrastructure BEFORE letting child run
    close(sync_pipe[0]);  // Close read end

    // Install signal handlers so Ctrl+C cleans up kernel profiling state
    g_interrupted = 0;
    struct sigaction old_sa[2];
    install_stop_handlers(old_sa);

    bool result = run_with_pet(state, pid, sample_period, verbose, sync_pipe[1],
                               -1, control, outcome);

    restore_stop_handlers(old_sa);

    if (g_interrupted) {
        fprintf(stderr, "\nInterrupted\n");
        // Reap the child if still running, so its status is still reportable
        kill(pid, SIGTERM);
        int status = 0;
        if (waitpid(pid, &status, 0) == pid) {
            outcome->reaped = true;
            outcome->status = status;
        }
    }

    return result;
}

// ============================================================================
// Output Formatting
// ============================================================================

typedef enum {
    OUTPUT_TEXT,
    OUTPUT_JSON
} output_format_t;

// The metric perf 6.x prints beside a counter in text output: GHz on cycles,
// insn per cycle on instructions (tools/perf/util/stat-shadow.c, print_cycles
// and print_instructions). `fmt` is perf's format for the value. perf 7.0
// prints no metric for events named with -e, in text or JSON, and mperf's JSON
// follows 7.0.
//
// GHz divides by task-clock, which is CPU time, not wall time: an N-threaded
// program accumulates cycles on N cores at once, so wall time would report N
// times the real clock. As in perf, it needs task-clock among the events.
//
// METRIC_UNAVAILABLE is perf's case where it names the unit but has no value,
// and leaves the column blank.
typedef enum {
    METRIC_NONE,         // perf prints no metric beside this counter
    METRIC_UNAVAILABLE,  // perf names the unit but has no value for it
    METRIC_OK
} metric_status_t;

static metric_status_t event_metric(pmc_state_t *state, const char *name,
                                    double *value_out, const char **fmt_out,
                                    const char **unit_out) {
    u64 cycles = 0, instructions = 0;
    double task_clock_ns = 0;
    for (int i = 0; i < state->event_count; i++) {
        if (strcasecmp(state->events[i].name, "cycles") == 0)
            cycles = state->events[i].value;
        if (strcasecmp(state->events[i].name, "instructions") == 0)
            instructions = state->events[i].value;
        if (state->events[i].kind == EVENT_TASK_CLOCK)
            task_clock_ns = (double)state->events[i].value;
    }

    *value_out = 0.0;

    // perf guards this on `cycles && nsecs`.
    if (strcasecmp(name, "cycles") == 0) {
        *fmt_out = "%8.3f";
        *unit_out = "GHz";
        if (cycles == 0 || task_clock_ns <= 0) return METRIC_UNAVAILABLE;
        // Cycles per nanosecond is already GHz.
        *value_out = (double)cycles / task_clock_ns;
        return METRIC_OK;
    }

    // perf guards this on the cycles counter alone, so asking for
    // instructions without cycles yields the unit with no value.
    if (strcasecmp(name, "instructions") == 0) {
        *fmt_out = "%7.2f ";
        *unit_out = "insn per cycle";
        if (cycles == 0) return METRIC_UNAVAILABLE;
        *value_out = (double)instructions / cycles;
        return METRIC_OK;
    }

    return METRIC_NONE;
}

static void output_text(pmc_state_t *state, char **cmd, FILE *out) {
    // Header mirrors perf stat's
    if (state->attached_pid > 0) {
        fprintf(out, "\n Performance counter stats for process id '%d'",
                state->attached_pid);
    } else {
        fprintf(out, "\n Performance counter stats for '");
        for (int i = 0; cmd[i]; i++)
            fprintf(out, "%s%s", i ? " " : "", cmd[i]);
        fprintf(out, "'");
    }
    fprintf(out, ":\n\n");

    for (int i = 0; i < state->event_count; i++) {
        configured_event_t *e = &state->events[i];
        // task-clock is perf's one msec event; the unit sits before the name
        if (e->kind == EVENT_TASK_CLOCK)
            fprintf(out, "  %'15.2f msec  %-24s", e->value / 1e6, e->name);
        else
            fprintf(out, "  %'20llu  %-24s", (unsigned long long)e->value, e->name);

        // perf lays the metric out as " # ", the value, a space, then the unit;
        // the widths come from its format strings, so the columns line up.
        double value;
        const char *fmt, *unit;
        if (event_metric(state, e->name, &value, &fmt, &unit) == METRIC_OK) {
            fprintf(out, "  # ");
            fprintf(out, fmt, value);
            fprintf(out, " %s", unit);
        }
        fprintf(out, "\n");
    }

    fprintf(out, "\n");
    fprintf(out, " %17.9f seconds time elapsed\n", state->wall_time_ns / 1e9);
    fprintf(out, "\n");
    if (state->attached_pid == 0) {
        fprintf(out, " %17.9f seconds user\n", state->user_time_ns / 1e9);
        fprintf(out, " %17.9f seconds sys\n", state->sys_time_ns / 1e9);
        fprintf(out, "\n");
    }
}

// perf stat --json emits newline-delimited JSON: one object per event, no
// enclosing array, no footer. The objects below are field for field what perf
// 7.0 writes for events named with -e, so a parser built for
// `perf stat --json` reads them unchanged:
//
//   {"counter-value" : "0.770947", "unit" : "msec", "event" : "task-clock", "event-runtime" : 770947, "pcnt-running" : 100.00}
//   {"counter-value" : "3650166.000000", "unit" : "", "event" : "cycles", "event-runtime" : 770947, "pcnt-running" : 100.00}
//   {"counter-value" : "1002571504.000000", "unit" : "", "event" : "duration_time", "event-runtime" : 1, "pcnt-running" : 100.00}
//
// event-runtime is how long the counter ran, in ns, which for a per-task
// counter is the task's time on CPU: task-clock. Tool events report 1. PET
// keeps every requested counter physically active for the whole run (there is
// no multiplexing, hence the hard limit of 10 events), so the running
// percentage is always 100. The elapsed/user/sys times come only as the tool
// events duration_time, user_time and system_time, as in perf.
static void output_json(pmc_state_t *state, FILE *out) {
    for (int i = 0; i < state->event_count; i++) {
        configured_event_t *e = &state->events[i];
        double value = (double)e->value;
        const char *unit = "";
        u64 runtime = state->task_clock_ns;

        if (e->kind == EVENT_TASK_CLOCK) {
            value /= 1e6;
            unit = "msec";
        } else if (e->kind == EVENT_DURATION_TIME || e->kind == EVENT_USER_TIME ||
                   e->kind == EVENT_SYSTEM_TIME) {
            runtime = 1;
        }

        fprintf(out, "{\"counter-value\" : \"%f\", \"unit\" : \"%s\", \"event\" : \"%s\", "
                     "\"event-runtime\" : %llu, \"pcnt-running\" : 100.00}\n",
                value, unit, e->name, (unsigned long long)runtime);
    }
}

// ============================================================================
// CLI Interface
// ============================================================================

// perf stat takes a comma-separated list as well as repeated -e flags, and
// harnesses tend to build one string from a list of events. Tokens point into
// a copy that deliberately outlives this call: `events` holds them until the
// PMU config is built. Empty tokens are skipped, so a trailing comma is fine.
static bool add_event_list(const char *spec, const char **events, int *count) {
    char *copy = strdup(spec);
    if (!copy) {
        fprintf(stderr, "Error: out of memory\n");
        return false;
    }

    for (char *tok = strtok(copy, ","); tok; tok = strtok(NULL, ",")) {
        if (*tok == '\0') continue;
        if (*count >= MAX_LISTED) {
            fprintf(stderr, "Too many events (max %d)\n", MAX_LISTED);
            return false;
        }
        events[(*count)++] = tok;
    }

    return true;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: sudo %s [options] -- command [args...]\n"
        "       sudo %s [options] -p PID\n"
        "\n"
        "Options:\n"
        "  -e, --event EVENT[,EVENT...]\n"
        "                       Events to measure (can repeat; at most 10\n"
        "                       hardware events, 16 in all)\n"
        "  -j, --json           Output newline-delimited JSON, one object per\n"
        "                       event, as perf stat --json does\n"
        "  -o, --output FILE    Write the report to FILE instead of stderr\n"
        "      --append         Append to the -o file instead of truncating\n"
        "      --log-fd FD      Write the report to file descriptor FD\n"
        "  -P, --period MS      PET sampling period in milliseconds (default: 1)\n"
        "  -p, --pid PID        Count an already-running process until it exits\n"
        "  -D, --delay -1       Start disabled; counting starts on \"enable\" from\n"
        "                       --control (only -1 is supported)\n"
        "      --control fifo:CTL[,ACK] | fd:CTL[,ACK]\n"
        "                       perf stat's control channel: \"enable\" starts\n"
        "                       counting, answered with \"ack\" once it has\n"
        "  -v, --verbose        Verbose output\n"
        "  -l, --list           List available events\n"
        "  -h, --help           Show this help\n"
        "\n"
        "The counter report goes to stderr (as perf stat does), so the measured\n"
        "program keeps sole ownership of stdout.\n"
        "\n"
        "The command runs as the invoking user (SUDO_UID under sudo, the real\n"
        "uid for a setuid install), and -o/--control paths are opened as that\n"
        "user. A setuid install may only -p the invoking user's processes.\n"
        "\n"
        "This tool uses PET (Profile Every Thread) to accurately measure\n"
        "multi-threaded programs. All threads in the target process are\n"
        "tracked and their counter values aggregated.\n"
        "\n"
        "Built-in event aliases (work on both Apple Silicon and Intel):\n"
        "  cycles, instructions, branches, branch-misses\n"
        "  l1d-tlb-misses, l1i-tlb-misses, l2-tlb-misses-data\n"
        "  l1d-cache-misses, l1i-cache-misses, llc-misses\n"
        "  ref-cycles (Intel only)\n"
        "  map-stalls, dispatch-stalls (Apple Silicon only)\n"
        "\n"
        "perf's software and tool events, from the kernel trace and the clock:\n"
        "  task-clock, page-faults (or faults)\n"
        "  duration_time, user_time, system_time\n"
        "\n"
        "Example:\n"
        "  sudo %s -e cycles -e instructions -- ./my_benchmark\n"
        "  sudo %s -j -e cycles -e l1d-tlb-misses -- ./ocaml_program\n"
        "\n",
        prog, prog, prog, prog);
}

static void list_events(void) {
    if (!load_frameworks()) return;

    kpep_db *db = NULL;
    if (kpep_db_create(NULL, &db) != 0) {
        fprintf(stderr, "Failed to load PMC database\n");
        return;
    }

    kpep_db_internal *db_int = (kpep_db_internal *)db;
    const char *arch_name;
#if defined(__arm64__)
    arch_name = "Apple Silicon (arm64)";
#else
    arch_name = "Intel (x86_64)";
#endif
    printf("Architecture:  %s\n", arch_name);
    printf("PMC Database:  %s (%s)\n",
           db_int->name ? db_int->name : "unknown",
           db_int->marketing_name ? db_int->marketing_name : "");
    printf("Fixed counters: %zu, Configurable: %zu\n\n",
           db_int->fixed_counter_count, db_int->config_counter_count);

    printf("Built-in aliases:\n");
    for (const event_alias_t *a = builtin_aliases; a->alias; a++) {
        kpep_event *ev = NULL;
        for (int i = 0; a->names[i]; i++) {
            if (kpep_db_event(db, a->names[i], &ev) == 0 && ev) {
                printf("  %-24s -> %s\n", a->alias, a->names[i]);
                break;
            }
        }
    }

    printf("\nAll available events:\n");
    usize count = 0;
    kpep_db_events_count(db, &count);

    kpep_event **events = calloc(count, sizeof(kpep_event *));
    if (events && kpep_db_events(db, events, count * sizeof(kpep_event *)) == 0) {
        for (usize i = 0; i < count; i++) {
            if (events[i] && events[i]->name) {
                printf("  %s", events[i]->name);
                if (events[i]->description) {
                    printf(" - %s", events[i]->description);
                }
                printf("\n");
            }
        }
    }
    free(events);
    kpep_db_free(db);
}

int main(int argc, char **argv) {
    // The %' thousands separators in the report are a no-op under the default C
    // locale, so adopt the environment's locale the way perf does.
    setlocale(LC_ALL, "");

    // Short flags follow perf stat where an equivalent exists. In particular
    // -p is perf's --pid, so the PET sampling period lives on -P/--period.
    enum { OPT_APPEND = 1000, OPT_LOG_FD, OPT_CONTROL };

    static struct option long_opts[] = {
        {"event",   required_argument, 0, 'e'},
        {"json",    no_argument,       0, 'j'},
        {"output",  required_argument, 0, 'o'},
        {"append",  no_argument,       0, OPT_APPEND},
        {"log-fd",  required_argument, 0, OPT_LOG_FD},
        {"period",  required_argument, 0, 'P'},
        {"pid",     required_argument, 0, 'p'},
        {"delay",   required_argument, 0, 'D'},
        {"control", required_argument, 0, OPT_CONTROL},
        {"verbose", no_argument,       0, 'v'},
        {"list",    no_argument,       0, 'l'},
        {"help",    no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    const char *events[MAX_LISTED];
    int event_count = 0;
    output_format_t format = OUTPUT_TEXT;
    double sample_period_ms = 1.0;  // 1ms default
    bool verbose = false;
    const char *output_name = NULL;
    int output_fd = -1;
    bool append = false;
    pid_t attach_pid = 0;
    bool delayed = false;
    const char *control_spec = NULL;

    int opt;
    while ((opt = getopt_long(argc, argv, "e:jo:P:p:D:vlh", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'e':
                if (!add_event_list(optarg, events, &event_count))
                    return 1;
                break;
            case 'j':
                format = OUTPUT_JSON;
                break;
            case 'o':
                output_name = optarg;
                break;
            case OPT_APPEND:
                append = true;
                break;
            case OPT_LOG_FD:
                output_fd = atoi(optarg);
                if (output_fd < 0) {
                    fprintf(stderr, "Invalid --log-fd: %s\n", optarg);
                    return 1;
                }
                break;
            case 'P':
                sample_period_ms = atof(optarg);
                if (sample_period_ms <= 0) {
                    fprintf(stderr, "Invalid sample period\n");
                    return 1;
                }
                break;
            case 'p': {
                unsigned long pid;
                if (!parse_id(optarg, &pid) || pid == 0 || pid > INT32_MAX) {
                    fprintf(stderr, "Invalid pid: %s\n"
                            "  (the PET sampling period is -P/--period MS)\n", optarg);
                    return 1;
                }
                attach_pid = (pid_t)pid;
                break;
            }
            case 'D':
                // perf also takes a positive delay in ms, we currently only support -1
                if (strcmp(optarg, "-1") != 0) {
                    fprintf(stderr, "Error: only --delay -1 is supported\n");
                    return 1;
                }
                delayed = true;
                break;
            case OPT_CONTROL:
                control_spec = optarg;
                break;
            case 'v':
                verbose = true;
                break;
            case 'l':
                list_events();
                return 0;
            case 'h':
            default:
                usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }

    if (attach_pid > 0 && optind < argc) {
        fprintf(stderr, "Error: give either -p PID or a command, not both\n");
        return 1;
    }
    if (attach_pid == 0 && optind >= argc) {
        fprintf(stderr, "Error: No command specified\n");
        usage(argv[0]);
        return 1;
    }

    if (output_name && output_fd >= 0) {
        fprintf(stderr, "Error: cannot use both --output and --log-fd\n");
        return 1;
    }

    if (delayed != (control_spec != NULL)) {
        fprintf(stderr, "Error: --delay -1 must go with --control\n");
        return 1;
    }

    // Before anything is opened by name: who those paths belong to.
    if (!invoker_init()) return 1;

    control_t control = { -1, -1 };
    if (control_spec && !parse_control(control_spec, &control)) return 1;

    // JSON numbers must use C formatting whatever the environment's locale is:
    // a comma decimal separator would emit "ipc": 1,364128 and break parsers.
    if (format == OUTPUT_JSON)
        setlocale(LC_NUMERIC, "C");

    // The report goes to stderr like perf stat, leaving stdout to the measured
    // program. Resolve any redirection now so a bad path fails before we run it.
    FILE *out = stderr;
    if (output_name) {
        int fd = open_as_invoker(output_name,
                                 O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC),
                                 0666);
        out = fd >= 0 ? fdopen(fd, append ? "a" : "w") : NULL;
        if (!out) {
            fprintf(stderr, "Error: cannot open %s: %s\n",
                    output_name, strerror(errno));
            return 1;
        }
    } else if (output_fd >= 0) {
        out = fdopen(output_fd, "w");
        if (!out) {
            fprintf(stderr, "Error: cannot use fd %d: %s\n",
                    output_fd, strerror(errno));
            return 1;
        }
    }

    // Default events
    if (event_count == 0) {
        events[event_count++] = "cycles";
        events[event_count++] = "instructions";
    }

    if (!load_frameworks()) return 1;

    pmc_state_t state;
    if (!pmc_init(&state)) return 1;

    for (int i = 0; i < event_count; i++) {
        if (!pmc_add_event(&state, events[i])) {
            pmc_cleanup(&state);
            return 1;
        }
    }
    // PET only reports threads it has counters for, and those threads are
    // what task-clock and page-faults are summed over.
    if (state.pmc_count == 0 && !pmc_add_counter(&state, "cycles", false)) {
        pmc_cleanup(&state);
        return 1;
    }

    if (!pmc_configure(&state)) {
        pmc_cleanup(&state);
        return 1;
    }

    if (verbose) {
        kpep_db_internal *db_int = (kpep_db_internal *)state.db;
        fprintf(stderr, "PMC database: %s\n", db_int->name);
        fprintf(stderr, "Counter count: %u\n", state.counter_count);
        fprintf(stderr, "Sample period: %.1f ms\n", sample_period_ms);
        fprintf(stderr, "Events: ");
        for (int i = 0; i < state.event_count; i++) {
            fprintf(stderr, "%s%s", state.events[i].name,
                    i < state.event_count - 1 ? ", " : "\n");
        }
    }

    double sample_period_sec = sample_period_ms / 1000.0;
    child_outcome_t outcome = { false, false, 0 };
    char pid_label[32];
    char *attach_argv[] = { pid_label, NULL };
    char **cmd_argv = &argv[optind];
    if (attach_pid > 0) {
        snprintf(pid_label, sizeof(pid_label), "pid %d", attach_pid);
        cmd_argv = attach_argv;
        state.attached_pid = attach_pid;

        // Watch for the exit before the ownership check, so a pid that is
        // recycled after the check is not the one we end up counting.
        int kq = kqueue();
        struct kevent ev;
        EV_SET(&ev, attach_pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
        if (kq < 0 || kevent(kq, &ev, 1, NULL, 0, NULL) < 0) {
            fprintf(stderr, "Error: cannot watch pid %d: %s\n", attach_pid, strerror(errno));
            pmc_cleanup(&state);
            return 1;
        }
        if (!may_attach(attach_pid)) {
            pmc_cleanup(&state);
            return 1;
        }

        g_interrupted = 0;
        struct sigaction old_sa[2];
        install_stop_handlers(old_sa);
        run_with_pet(&state, attach_pid, sample_period_sec, verbose, -1, kq,
                     &control, &outcome);
        restore_stop_handlers(old_sa);
        close(kq);

        if (g_interrupted) outcome.reaped = true;
    } else {
        run_command_pet(&state, cmd_argv, sample_period_sec, verbose, &control,
                        &outcome);
    }

    // Exit with the command's own status, as perf stat does: its exit code when
    // it exited normally, 128 + signal when it was killed. A tool failure that
    // stopped us reaching the command at all reports 1.
    int exit_code = 1;
    if (outcome.reaped) {
        if (WIFSIGNALED(outcome.status)) {
            psignal(WTERMSIG(outcome.status), cmd_argv[0]);
            exit_code = 128 + WTERMSIG(outcome.status);
        } else {
            exit_code = WEXITSTATUS(outcome.status);
        }
    }

    // Like perf, a run that never counted leaves no report, only the error.
    if (!outcome.counted) {
        exit_code = 1;
    } else if (format == OUTPUT_JSON) {
        output_json(&state, out);
    } else {
        output_text(&state, cmd_argv, out);
    }
    fflush(out);
    if (out != stderr) fclose(out);

    pmc_cleanup(&state);
    return exit_code;
}