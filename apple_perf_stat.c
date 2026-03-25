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
#include <getopt.h>
#include <signal.h>
#include <errno.h>

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

#define MAX_EVENTS 10

typedef struct {
    const char *name;
    kpep_event *event;
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

    configured_event_t events[MAX_EVENTS];
    int event_count;

    double wall_time_ns;
    double user_time_ns;
    double sys_time_ns;

    // PET-specific
    int num_threads_seen;
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

static bool pmc_add_event(pmc_state_t *state, const char *name) {
    if (state->event_count >= MAX_EVENTS) {
        fprintf(stderr, "Error: Too many events (max %d)\n", MAX_EVENTS);
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

    state->events[state->event_count].name = name;
    state->events[state->event_count].event = ev;
    state->event_count++;

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

// ============================================================================
// PET-based measurement (accurate, multi-threaded)
// ============================================================================

static bool run_with_pet(pmc_state_t *state, pid_t target_pid,
                         double sample_period_sec, bool verbose, int sync_fd) {
    bool success = false;
    int ret;

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

    // Filter to only PMC thread data
    kd_regtype kdr = { 0 };
    kdr.type = KDBG_VALCHECK;
    kdr.value1 = KDBG_EVENTID(DBG_PERF, PERF_KPC, PERF_KPC_DATA_THREAD);
    if ((ret = kdebug_setreg(&kdr))) {
        fprintf(stderr, "Error: Failed to set kdebug filter: %d\n", ret);
        goto fail_early;
    }
    if ((ret = kdebug_trace_enable(1))) {
        fprintf(stderr, "Error: Failed to enable kdebug: %d\n", ret);
        goto fail_early;
    }

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
        goto cleanup;
    }

    usize total_raw_samples = 0;
    usize total_filtered_samples = 0;

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

        // Check if child exited
        pid_t result = waitpid(target_pid, &status, WNOHANG);
        if (result == -1 && errno != EINTR) break;
        if (result == target_pid) {
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

        // Read trace data
        usize count = 0;
        kdebug_trace_read(buf_cur, sizeof(kd_buf) * nbufs, &count);
        total_raw_samples += count;

        // Filter for PMC thread data
        kd_buf *write_ptr = buf_cur;
        for (kd_buf *buf = buf_cur, *end = buf_cur + count; buf < end; buf++) {
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
        for (int i = 0; i < state->event_count; i++) {
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

    state->num_threads_seen = threads_with_data;

    // Map counter values to events
    for (int i = 0; i < state->event_count; i++) {
        usize idx = state->counter_map[i];
        state->events[i].value = total_counters[idx];
    }

    // Compute times
    state->wall_time_ns =
        (wall_end.tv_sec - wall_start.tv_sec) * 1e9 +
        (wall_end.tv_usec - wall_start.tv_usec) * 1e3;

    struct timeval user_diff, sys_diff;
    timersub(&usage_end.ru_utime, &usage_start.ru_utime, &user_diff);
    timersub(&usage_end.ru_stime, &usage_start.ru_stime, &sys_diff);

    state->user_time_ns = user_diff.tv_sec * 1e9 + user_diff.tv_usec * 1e3;
    state->sys_time_ns = sys_diff.tv_sec * 1e9 + sys_diff.tv_usec * 1e3;

    success = WIFEXITED(status) && WEXITSTATUS(status) == 0;

cleanup:
    profiling_cleanup();
    return success;

fail_early:
    if (sync_fd >= 0) close(sync_fd);
    profiling_cleanup();
    return false;
}

static bool run_command_pet(pmc_state_t *state, char **argv, double sample_period, bool verbose) {
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
        // Child: wait for parent to set up profiling
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

    // Install signal handler so Ctrl+C cleans up kernel profiling state
    g_interrupted = 0;
    struct sigaction sa = { .sa_handler = sigint_handler };
    struct sigaction old_sa;
    sigaction(SIGINT, &sa, &old_sa);

    bool result = run_with_pet(state, pid, sample_period, verbose, sync_pipe[1]);

    // Restore previous signal handler
    sigaction(SIGINT, &old_sa, NULL);

    if (g_interrupted) {
        fprintf(stderr, "\nInterrupted\n");
        // Reap the child if still running
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
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

static void output_text(pmc_state_t *state) {
    printf("\n Performance counter stats");
    if (state->num_threads_seen > 0) {
        printf(" (%d thread%s)", state->num_threads_seen,
               state->num_threads_seen == 1 ? "" : "s");
    }
    printf(":\n\n");

    u64 cycles = 0, instructions = 0;
    for (int i = 0; i < state->event_count; i++) {
        if (strcasecmp(state->events[i].name, "cycles") == 0)
            cycles = state->events[i].value;
        if (strcasecmp(state->events[i].name, "instructions") == 0)
            instructions = state->events[i].value;
    }

    for (int i = 0; i < state->event_count; i++) {
        configured_event_t *e = &state->events[i];
        printf("  %'20llu  %-24s", (unsigned long long)e->value, e->name);
        if (strcasecmp(e->name, "instructions") == 0 && cycles > 0) {
            printf("  # %6.2f IPC", (double)instructions / cycles);
        }
        printf("\n");
    }

    printf("\n");
    printf("  %'14.6f seconds wall time\n", state->wall_time_ns / 1e9);
    printf("  %'14.6f seconds user\n", state->user_time_ns / 1e9);
    printf("  %'14.6f seconds sys\n", state->sys_time_ns / 1e9);
    printf("\n");
}

static void output_json(pmc_state_t *state) {
    printf("{\n");
    printf("  \"counters\": {\n");

    for (int i = 0; i < state->event_count; i++) {
        configured_event_t *e = &state->events[i];
        printf("    \"%s\": %llu%s\n",
               e->name,
               (unsigned long long)e->value,
               i < state->event_count - 1 ? "," : "");
    }

    printf("  },\n");
    printf("  \"time\": {\n");
    printf("    \"wall_ns\": %.0f,\n", state->wall_time_ns);
    printf("    \"user_ns\": %.0f,\n", state->user_time_ns);
    printf("    \"sys_ns\": %.0f\n", state->sys_time_ns);
    printf("  },\n");

    printf("  \"derived\": {\n");
    u64 cycles = 0, instructions = 0;
    for (int i = 0; i < state->event_count; i++) {
        if (strcasecmp(state->events[i].name, "cycles") == 0)
            cycles = state->events[i].value;
        if (strcasecmp(state->events[i].name, "instructions") == 0)
            instructions = state->events[i].value;
    }

    if (cycles > 0 && instructions > 0) {
        printf("    \"ipc\": %.6f,\n", (double)instructions / cycles);
        printf("    \"cpi\": %.6f\n", (double)cycles / instructions);
    }
    printf("  },\n");

    printf("  \"threads_measured\": %d\n", state->num_threads_seen);
    printf("}\n");
}

// ============================================================================
// CLI Interface
// ============================================================================

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: sudo %s [options] -- command [args...]\n"
        "\n"
        "Options:\n"
        "  -e, --event EVENT    Add event to measure (can repeat, max 10)\n"
        "  -j, --json           Output in JSON format\n"
        "  -p, --period MS      Sampling period in milliseconds (default: 1)\n"
        "  -v, --verbose        Verbose output\n"
        "  -l, --list           List available events\n"
        "  -h, --help           Show this help\n"
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
        "Example:\n"
        "  sudo %s -e cycles -e instructions -- ./my_benchmark\n"
        "  sudo %s -j -e cycles -e l1d-tlb-misses -- ./ocaml_program\n"
        "\n",
        prog, prog, prog);
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
    static struct option long_opts[] = {
        {"event",   required_argument, 0, 'e'},
        {"json",    no_argument,       0, 'j'},
        {"period",  required_argument, 0, 'p'},
        {"verbose", no_argument,       0, 'v'},
        {"list",    no_argument,       0, 'l'},
        {"help",    no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    const char *events[MAX_EVENTS];
    int event_count = 0;
    output_format_t format = OUTPUT_TEXT;
    double sample_period_ms = 1.0;  // 1ms default
    bool verbose = false;

    int opt;
    while ((opt = getopt_long(argc, argv, "e:jp:vlh", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'e':
                if (event_count >= MAX_EVENTS) {
                    fprintf(stderr, "Too many events (max %d)\n", MAX_EVENTS);
                    return 1;
                }
                events[event_count++] = optarg;
                break;
            case 'j':
                format = OUTPUT_JSON;
                break;
            case 'p':
                sample_period_ms = atof(optarg);
                if (sample_period_ms <= 0) {
                    fprintf(stderr, "Invalid sample period\n");
                    return 1;
                }
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

    if (optind >= argc) {
        fprintf(stderr, "Error: No command specified\n");
        usage(argv[0]);
        return 1;
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
    bool success = run_command_pet(&state, &argv[optind], sample_period_sec, verbose);

    if (format == OUTPUT_JSON) {
        output_json(&state);
    } else {
        output_text(&state);
    }

    pmc_cleanup(&state);
    return success ? 0 : 1;
}