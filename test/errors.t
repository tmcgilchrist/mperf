Argument errors are all caught before mperf-stat loads the PMC frameworks, so
none of these need root.

Unknown options and missing arguments get getopt's own message (worded by the
platform's libc), then the usage text.

  $ mperf-stat --frobnicate -- true
  mperf-stat: unrecognized option `--frobnicate'
  Usage: sudo mperf-stat [options] -- command [args...]
  
  Options:
    -e, --event EVENT[,EVENT...]
                         Events to measure (can repeat, max 10 total)
    -j, --json           Output newline-delimited JSON, one object per
                         counter, as perf stat --json does
    -o, --output FILE    Write the report to FILE instead of stderr
        --append         Append to the -o file instead of truncating
        --log-fd FD      Write the report to file descriptor FD
    -P, --period MS      PET sampling period in milliseconds (default: 1)
    -v, --verbose        Verbose output
    -l, --list           List available events
    -h, --help           Show this help
  
  The counter report goes to stderr (as perf stat does), so the measured
  program keeps sole ownership of stdout.
  
  Reserved to match perf stat, not supported yet:
    -p, --pid PID        Attach to an already-running process
  
  This tool uses PET (Profile Every Thread) to accurately measure
  multi-threaded programs. All threads in the target process are
  tracked and their counter values aggregated.
  
  Built-in event aliases (work on both Apple Silicon and Intel):
    cycles, instructions, branches, branch-misses
    l1d-tlb-misses, l1i-tlb-misses, l2-tlb-misses-data
    l1d-cache-misses, l1i-cache-misses, llc-misses
    ref-cycles (Intel only)
    map-stalls, dispatch-stalls (Apple Silicon only)
  
  Example:
    sudo mperf-stat -e cycles -e instructions -- ./my_benchmark
    sudo mperf-stat -j -e cycles -e l1d-tlb-misses -- ./ocaml_program
  
  [1]

  $ mperf-stat -x -- true
  mperf-stat: invalid option -- x
  Usage: sudo mperf-stat [options] -- command [args...]
  
  Options:
    -e, --event EVENT[,EVENT...]
                         Events to measure (can repeat, max 10 total)
    -j, --json           Output newline-delimited JSON, one object per
                         counter, as perf stat --json does
    -o, --output FILE    Write the report to FILE instead of stderr
        --append         Append to the -o file instead of truncating
        --log-fd FD      Write the report to file descriptor FD
    -P, --period MS      PET sampling period in milliseconds (default: 1)
    -v, --verbose        Verbose output
    -l, --list           List available events
    -h, --help           Show this help
  
  The counter report goes to stderr (as perf stat does), so the measured
  program keeps sole ownership of stdout.
  
  Reserved to match perf stat, not supported yet:
    -p, --pid PID        Attach to an already-running process
  
  This tool uses PET (Profile Every Thread) to accurately measure
  multi-threaded programs. All threads in the target process are
  tracked and their counter values aggregated.
  
  Built-in event aliases (work on both Apple Silicon and Intel):
    cycles, instructions, branches, branch-misses
    l1d-tlb-misses, l1i-tlb-misses, l2-tlb-misses-data
    l1d-cache-misses, l1i-cache-misses, llc-misses
    ref-cycles (Intel only)
    map-stalls, dispatch-stalls (Apple Silicon only)
  
  Example:
    sudo mperf-stat -e cycles -e instructions -- ./my_benchmark
    sudo mperf-stat -j -e cycles -e l1d-tlb-misses -- ./ocaml_program
  
  [1]

  $ mperf-stat -e
  mperf-stat: option requires an argument -- e
  Usage: sudo mperf-stat [options] -- command [args...]
  
  Options:
    -e, --event EVENT[,EVENT...]
                         Events to measure (can repeat, max 10 total)
    -j, --json           Output newline-delimited JSON, one object per
                         counter, as perf stat --json does
    -o, --output FILE    Write the report to FILE instead of stderr
        --append         Append to the -o file instead of truncating
        --log-fd FD      Write the report to file descriptor FD
    -P, --period MS      PET sampling period in milliseconds (default: 1)
    -v, --verbose        Verbose output
    -l, --list           List available events
    -h, --help           Show this help
  
  The counter report goes to stderr (as perf stat does), so the measured
  program keeps sole ownership of stdout.
  
  Reserved to match perf stat, not supported yet:
    -p, --pid PID        Attach to an already-running process
  
  This tool uses PET (Profile Every Thread) to accurately measure
  multi-threaded programs. All threads in the target process are
  tracked and their counter values aggregated.
  
  Built-in event aliases (work on both Apple Silicon and Intel):
    cycles, instructions, branches, branch-misses
    l1d-tlb-misses, l1i-tlb-misses, l2-tlb-misses-data
    l1d-cache-misses, l1i-cache-misses, llc-misses
    ref-cycles (Intel only)
    map-stalls, dispatch-stalls (Apple Silicon only)
  
  Example:
    sudo mperf-stat -e cycles -e instructions -- ./my_benchmark
    sudo mperf-stat -j -e cycles -e l1d-tlb-misses -- ./ocaml_program
  
  [1]

-p is reserved for perf's --pid and is not implemented; it points at -P.

  $ mperf-stat -p 123 -- true
  Error: -p/--pid (attach to an already-running process) is not supported yet.
         To set the PET sampling period, use -P/--period MS.
  [1]

  $ mperf-stat --pid 123 -- true
  Error: -p/--pid (attach to an already-running process) is not supported yet.
         To set the PET sampling period, use -P/--period MS.
  [1]

The PET sampling period must be a positive number of milliseconds.

  $ mperf-stat -P 0 -- true
  Invalid sample period
  [1]

  $ mperf-stat -P -1 -- true
  Invalid sample period
  [1]

  $ mperf-stat --period abc -- true
  Invalid sample period
  [1]

--log-fd must not be negative.

  $ mperf-stat --log-fd -1 -- true
  Invalid --log-fd: -1
  [1]

The report can go to a file or to a descriptor, not both. This is checked
after the command, so one has to be given.

  $ mperf-stat --output report.txt --log-fd 3 -- true
  Error: cannot use both --output and --log-fd
  [1]

At most 10 events, whether given as one comma-separated list or as repeated -e
flags.

  $ mperf-stat -e cycles,instructions,branches,branch-misses,l1d-tlb-misses,l1i-tlb-misses,l2-tlb-misses-data,l1d-cache-misses,l1i-cache-misses,llc-misses,cycles -- true
  Too many events (max 10)
  [1]

  $ mperf-stat -e cycles,instructions,branches,branch-misses,l1d-tlb-misses -e l1i-tlb-misses,l2-tlb-misses-data,l1d-cache-misses,l1i-cache-misses,llc-misses -e cycles -- true
  Too many events (max 10)
  [1]
