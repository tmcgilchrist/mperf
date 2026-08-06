# mperf

mperf - A `perf stat`-like tool for macOS (Apple Silicon and Intel).

## Overview

This tool provides access to hardware performance counters (PMCs) on macOS via a simple CLI interface, similar to Linux's `perf stat`. It works on both Apple Silicon (M1/M2/M3/M4) and Intel Macs.

Linux `perf stat` uses exact hardware counting when the number of requested events fits within the CPU's physical PMU counters (typically 4-8 on x86_64, ~7 on ARM64). Beyond that limit, the kernel enables time-division multiplexing and scales the counts — producing estimates, not exact values. mperf's PET approach is fundamentally sampling-based: all requested counters (up to 10) are always physically active, but values are captured via periodic kernel snapshots rather than continuous counting.

## Permissions & Security

### Why Root is Required

The kpc (kernel performance counters) APIs used by this tool require root privileges. This is enforced by XNU (the macOS kernel) via the `kpc_force_all_ctrs_set` sysctl.

```
$ ./mperf-stat -e cycles -- echo hello
Error: Root privileges required (run with sudo)

$ sudo ./mperf-stat -e cycles -- echo hello
hello

 Performance counter stats:

             1,234,567  cycles
```

### What's NOT Required

| Requirement          | Needed? |
|----------------------|---------|
| Root/sudo            | ✅ Yes  |
| Disable SIP          | ❌ No   |
| Code signing         | ❌ No   |
| Notarization         | ❌ No   |
| Kernel extension     | ❌ No   |
| Special entitlements | ❌ No   |

### Security Notes

- The tool uses Apple's private `kperf.framework` and `kperfdata.framework`
- These are undocumented APIs that could change with macOS updates
- The tool only reads performance counters; it doesn't modify system state

## Building

```bash
# Build the C tool
make

# Install to /usr/local/bin
sudo make install

# Build OCaml library
dune build
```

## Usage

### CLI Tool

```bash
# Basic usage (defaults to cycles + instructions)
sudo ./mperf-stat -- ./my_benchmark

# Specify events
sudo ./mperf-stat -e cycles -e instructions -e l1d-tlb-misses -- ./benchmark

# JSON output (for scripting/parsing)
sudo ./mperf-stat -j -e cycles -e instructions -- ./benchmark

# List available events
./mperf-stat -l
```

### Example Output

**Text format:**
```
 Performance counter stats:

         1,234,567,890  cycles
         2,345,678,901  instructions                # 1.90 IPC
            12,345,678  l1d-tlb-misses

       0.543210 seconds wall time
       0.520000 seconds user
       0.020000 seconds sys
```

**JSON format:**
```json
{
  "counters": {
    "cycles": 1234567890,
    "instructions": 2345678901,
    "l1d-tlb-misses": 12345678
  },
  "time": {
    "wall_ns": 543210000,
    "user_ns": 520000000,
    "sys_ns": 20000000
  },
  "derived": {
    "ipc": 1.9000,
    "cpi": 0.5263
  }
}
```

### OCaml Library

An OCaml wrapper is provided for future integration with OCaml benchmarking tooling.

```ocaml
open Apple_perf_stat

let () =
  (* Must run as root *)
  match run
    ~events:["cycles"; "instructions"; "l1d-tlb-misses"; "branch-misses"]
    ["./my_benchmark"; "--size"; "10000"]
  with
  | Ok result ->
    Printf.printf "IPC: %.2f\n" (get_ipc result);
    Printf.printf "Wall time: %.3f s\n" (wall_time_seconds result);

    (* Access specific counters *)
    (match get_counter "l1d-tlb-misses" result with
     | Some misses -> Printf.printf "TLB misses: %Ld\n" misses
     | None -> ());

    (* Pretty print everything *)
    Format.printf "%a\n" pp_result result

  | Error e ->
    Printf.eprintf "Error: %s\n" (string_of_error e);
    exit 1
```

## Available Events

### Built-in Aliases

These aliases work across Apple Silicon and Intel:

| Alias                | Description                 | Availability |
|----------------------|-----------------------------|--------------|
| `cycles`             | CPU cycles                  | Both         |
| `instructions`       | Retired instructions        | Both         |
| `branches`           | Branch instructions         | Both         |
| `branch-misses`      | Mispredicted branches       | Both         |
| `l1d-tlb-misses`     | L1 data TLB misses          | Both         |
| `l1i-tlb-misses`     | L1 instruction TLB misses   | Both         |
| `l2-tlb-misses-data` | L2 TLB data misses          | Both         |
| `l1d-cache-misses`   | L1 data cache misses        | Both         |
| `l1i-cache-misses`   | L1 instruction cache misses | Both         |
| `llc-misses`         | Last-level cache misses     | Both         |
| `ref-cycles`         | Reference cycles (fixed)    | Intel only   |
| `map-stalls`         | Map unit stall cycles       | Apple Silicon only |
| `dispatch-stalls`    | Dispatch stall cycles       | Apple Silicon only |

### Raw Events

You can also use raw event names from the PMC database. The database files are at `/usr/share/kpep/`:

| CPU               | Database File               |
|-------------------|-----------------------------|
| M1 (all variants) | `a14.plist`                 |
| M2 (all variants) | `a15.plist`                 |
| M3                | `as1.plist`                 |
| M3 Pro/Max        | `as3.plist`                 |
| M4                | `as4.plist`                 |
| Intel             | `cpu_*.plist` (varies by model) |

Run `./mperf-stat -l` to see all events for your CPU.

## How It Works: PET (Profile Every Thread)

Programs can have many pthreads. A naive approach of measuring just one thread would miss most of the work.

This tool uses Apple's **Profile Every Thread (PET)** mechanism:

1. **Timer-based sampling**: A kernel timer fires every N milliseconds (default: 1ms)
2. **Per-thread snapshots**: On each tick, the kernel samples PMC values for ALL threads matching our PID filter
3. **kdebug tracing**: Samples are written to a kernel trace buffer with thread IDs
4. **Aggregation**: We track first/last sample per thread, compute deltas, sum across all threads

```
Thread 1: [sample_0] -------- [sample_1] -------- [sample_N]
Thread 2:      [sample_0] -------- [sample_1] -------- [sample_N]
Thread 3:           [sample_0] -------- [sample_1] -------- [sample_N]
...

Result = Σ (thread_last - thread_first) for all threads
```

### Sampling Period Trade-offs

```bash
# Default: 1ms period - good balance
sudo ./mperf-stat -e cycles -e instructions -- ./benchmark

# Faster sampling (0.5ms) - more accurate for short-lived threads
sudo ./mperf-stat -P 0.5 -e cycles -e instructions -- ./short_benchmark

# Slower sampling (5ms) - less overhead for long-running programs
sudo ./mperf-stat -P 5 -e cycles -e instructions -- ./long_benchmark
```

- **Faster sampling:** More accurate (catches short-lived threads) but higher overhead
- **Slower sampling:** Less overhead but may miss threads that live < sample_period

### What Gets Measured

| Entity          | Measured?  | Notes                         |
|-----------------|------------|-------------------------------|
| Main thread     | ✅ Yes     |                               |
| OCaml domains   | ✅ Yes     | All domain threads            |
| Systhreads      | ✅ Yes     | OCaml Thread module           |
| C pthreads      | ✅ Yes     | From FFI/bindings             |
| Child processes | ❌ No      | Only target PID               |
| Kernel time     | ⚠️ Partial | Counts if thread is scheduled |

## Limitations

### Counter Limits

The number of hardware counters varies by architecture:

| Architecture  | Fixed Counters | Configurable Counters | Max Events |
|---------------|----------------|-----------------------|------------|
| Apple Silicon | 2              | 8                     | 10         |
| Intel         | 3              | 4-8 (varies by model) | 7-11       |

Run `./mperf-stat -l` to see the counter counts for your CPU.

### Sampling Accuracy

PET is sampling-based, not exact counting:
- Very short-lived threads (< sampling period) may be missed entirely
- Counter values are aggregated from samples, not exact totals
- For very short benchmarks, it would be better to run multiple iterations

### Thread Limits

The tool tracks up to 256 unique threads. For programs with more threads, some data may be lost (with a warning).

### Known Issues

- **M4 MacBook Pro bug**: There was a kernel bug causing `kpc_set_config` to fail on some M4 MacBook Pro models. This appears to be fixed in macOS 15.4+.
- **Very short programs**: Programs completing in < 2× sample period may have no samples. Increase iterations or decrease sample period.

## Comparison with Linux perf stat

| Feature        | perf stat (Linux)                          | mperf-stat (macOS)                        |
|----------------|--------------------------------------------|-------------------------------------------|
| Invocation     | `perf stat -e events -- cmd`               | `sudo mperf-stat -e events -- cmd`        |
| Counting       | Exact when within HW counter limit; scaled estimates when multiplexing | Sampling-based (PET), all counters always active |
| Root required  | Some events                                | All events                                |
| Output formats | Text, JSON, CSV                            | Text, JSON                                |
| Report stream  | stderr; `-o`, `--append`, `--log-fd`       | Same                                      |
| HW counters    | 4-8 GP + 3-4 fixed (varies by CPU)        | Varies: 2-3 fixed + 4-8 configurable      |
| Multiplexing   | Yes, with time-scaled estimates            | No (hard limit of 10 events)              |
| Process scope  | Direct measurement                         | PID-filtered kernel sampling              |
| Event naming   | Standardised                               | Apple-specific + portable aliases         |
| API            | Public `perf_event_open` syscall           | Private frameworks (may change)           |

## Credits

Based on [ibireme's kpc_demo.c](https://gist.github.com/ibireme/173517c208c7dc333ba962c1f0d67d12) (public domain).

PMC event documentation from:
- [jiegec/apple-pmu](https://github.com/jiegec/apple-pmu)
- [Apple Silicon CPU Optimization Guide](https://developer.apple.com/documentation/apple-silicon/cpu-optimization-guide)

## License

MIT


TODO

 - [x] Port to x86_64 macOS versions, there should be similar PMC counters available.
