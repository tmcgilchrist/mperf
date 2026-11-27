Argument errors are all caught before mperf-stat loads the PMC frameworks, so
none of these need root.

Unknown options and missing arguments get getopt's own message (worded by the
platform's libc), then the usage text.

  $ mperf-stat --frobnicate -- true
  mperf-stat: unrecognized option `--frobnicate'
  Usage: sudo mperf-stat [options] -- command [args...]
         sudo mperf-stat [options] -p PID
  
  Options:
    -e, --event EVENT[,EVENT...]
                         Events to measure (can repeat; at most 10
                         hardware events, 16 in all)
    -j, --json           Output newline-delimited JSON, one object per
                         event, as perf stat --json does
    -o, --output FILE    Write the report to FILE instead of stderr
        --append         Append to the -o file instead of truncating
        --log-fd FD      Write the report to file descriptor FD
    -P, --period MS      PET sampling period in milliseconds (default: 1)
    -p, --pid PID        Count an already-running process until it exits
    -D, --delay -1       Start disabled; counting starts on "enable" from
                         --control (only -1 is supported)
        --control fifo:CTL[,ACK] | fd:CTL[,ACK]
                         perf stat's control channel: "enable" starts
                         counting, answered with "ack" once it has;
                         "disable" ends it
    -v, --verbose        Verbose output
    -l, --list           List available events
    -h, --help           Show this help
  
  The counter report goes to stderr (as perf stat does), so the measured
  program keeps sole ownership of stdout.
  
  The command runs as the invoking user (SUDO_UID under sudo, the real
  uid for a setuid install), and -o/--control paths are opened as that
  user. A setuid install may only -p the invoking user's processes.
  
  This tool uses PET (Profile Every Thread) to accurately measure
  multi-threaded programs. All threads in the target process are
  tracked and their counter values aggregated.
  
  Built-in event aliases (work on both Apple Silicon and Intel):
    cycles, instructions, branches, branch-misses
    l1d-tlb-misses, l1i-tlb-misses, l2-tlb-misses-data
    l1d-cache-misses, l1i-cache-misses, llc-misses
    ref-cycles (Intel only)
    map-stalls, dispatch-stalls (Apple Silicon only)
  
  perf's software and tool events, from the kernel trace and the clock:
    task-clock, page-faults (or faults)
    duration_time, user_time, system_time
  
  Example:
    sudo mperf-stat -e cycles -e instructions -- ./my_benchmark
    sudo mperf-stat -j -e cycles -e l1d-tlb-misses -- ./ocaml_program
  
  [1]

  $ mperf-stat -x -- true
  mperf-stat: invalid option -- x
  Usage: sudo mperf-stat [options] -- command [args...]
         sudo mperf-stat [options] -p PID
  
  Options:
    -e, --event EVENT[,EVENT...]
                         Events to measure (can repeat; at most 10
                         hardware events, 16 in all)
    -j, --json           Output newline-delimited JSON, one object per
                         event, as perf stat --json does
    -o, --output FILE    Write the report to FILE instead of stderr
        --append         Append to the -o file instead of truncating
        --log-fd FD      Write the report to file descriptor FD
    -P, --period MS      PET sampling period in milliseconds (default: 1)
    -p, --pid PID        Count an already-running process until it exits
    -D, --delay -1       Start disabled; counting starts on "enable" from
                         --control (only -1 is supported)
        --control fifo:CTL[,ACK] | fd:CTL[,ACK]
                         perf stat's control channel: "enable" starts
                         counting, answered with "ack" once it has;
                         "disable" ends it
    -v, --verbose        Verbose output
    -l, --list           List available events
    -h, --help           Show this help
  
  The counter report goes to stderr (as perf stat does), so the measured
  program keeps sole ownership of stdout.
  
  The command runs as the invoking user (SUDO_UID under sudo, the real
  uid for a setuid install), and -o/--control paths are opened as that
  user. A setuid install may only -p the invoking user's processes.
  
  This tool uses PET (Profile Every Thread) to accurately measure
  multi-threaded programs. All threads in the target process are
  tracked and their counter values aggregated.
  
  Built-in event aliases (work on both Apple Silicon and Intel):
    cycles, instructions, branches, branch-misses
    l1d-tlb-misses, l1i-tlb-misses, l2-tlb-misses-data
    l1d-cache-misses, l1i-cache-misses, llc-misses
    ref-cycles (Intel only)
    map-stalls, dispatch-stalls (Apple Silicon only)
  
  perf's software and tool events, from the kernel trace and the clock:
    task-clock, page-faults (or faults)
    duration_time, user_time, system_time
  
  Example:
    sudo mperf-stat -e cycles -e instructions -- ./my_benchmark
    sudo mperf-stat -j -e cycles -e l1d-tlb-misses -- ./ocaml_program
  
  [1]

  $ mperf-stat -e
  mperf-stat: option requires an argument -- e
  Usage: sudo mperf-stat [options] -- command [args...]
         sudo mperf-stat [options] -p PID
  
  Options:
    -e, --event EVENT[,EVENT...]
                         Events to measure (can repeat; at most 10
                         hardware events, 16 in all)
    -j, --json           Output newline-delimited JSON, one object per
                         event, as perf stat --json does
    -o, --output FILE    Write the report to FILE instead of stderr
        --append         Append to the -o file instead of truncating
        --log-fd FD      Write the report to file descriptor FD
    -P, --period MS      PET sampling period in milliseconds (default: 1)
    -p, --pid PID        Count an already-running process until it exits
    -D, --delay -1       Start disabled; counting starts on "enable" from
                         --control (only -1 is supported)
        --control fifo:CTL[,ACK] | fd:CTL[,ACK]
                         perf stat's control channel: "enable" starts
                         counting, answered with "ack" once it has;
                         "disable" ends it
    -v, --verbose        Verbose output
    -l, --list           List available events
    -h, --help           Show this help
  
  The counter report goes to stderr (as perf stat does), so the measured
  program keeps sole ownership of stdout.
  
  The command runs as the invoking user (SUDO_UID under sudo, the real
  uid for a setuid install), and -o/--control paths are opened as that
  user. A setuid install may only -p the invoking user's processes.
  
  This tool uses PET (Profile Every Thread) to accurately measure
  multi-threaded programs. All threads in the target process are
  tracked and their counter values aggregated.
  
  Built-in event aliases (work on both Apple Silicon and Intel):
    cycles, instructions, branches, branch-misses
    l1d-tlb-misses, l1i-tlb-misses, l2-tlb-misses-data
    l1d-cache-misses, l1i-cache-misses, llc-misses
    ref-cycles (Intel only)
    map-stalls, dispatch-stalls (Apple Silicon only)
  
  perf's software and tool events, from the kernel trace and the clock:
    task-clock, page-faults (or faults)
    duration_time, user_time, system_time
  
  Example:
    sudo mperf-stat -e cycles -e instructions -- ./my_benchmark
    sudo mperf-stat -j -e cycles -e l1d-tlb-misses -- ./ocaml_program
  
  [1]

-p attaches to a running process, so it cannot also be given a command, and
it needs a positive pid. The PET sampling period is -P.

  $ mperf-stat -p 123 -- true
  Error: give either -p PID or a command, not both
  [1]

  $ mperf-stat --pid 123 -- true
  Error: give either -p PID or a command, not both
  [1]

  $ mperf-stat -p 0
  Invalid pid: 0
    (the PET sampling period is -P/--period MS)
  [1]

  $ mperf-stat -p abc
  Invalid pid: abc
    (the PET sampling period is -P/--period MS)
  [1]

Only perf's --delay -1 is supported, and it goes with --control.

  $ mperf-stat --delay 100 -- true
  Error: only --delay -1 is supported
  [1]

  $ mperf-stat --delay -1 -- true
  Error: --delay -1 must go with --control
  [1]

  $ mperf-stat --control fifo:ctl -- true
  Error: --delay -1 must go with --control
  [1]

--control takes fifo:CTL[,ACK] or fd:CTL[,ACK], and an fd must be open.

  $ mperf-stat --delay -1 --control pipe:ctl -- true
  Error: --control takes fifo:CTL[,ACK] or fd:CTL[,ACK]
  [1]

  $ mperf-stat --delay -1 --control fd:4294967299 -- true
  Error: bad --control fd spec: fd:4294967299
  [1]

  $ mperf-stat --delay -1 --control fd:250 -- true
  Error: --control fd 250 is not open
  Error: bad --control fd spec: fd:250
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

-e names at most 16 events, whether given as one comma-separated list or as
repeated -e flags. (The limit of 10 hardware events is checked once the PMC
database is loaded, which needs root.)

  $ mperf-stat -e cycles,instructions,branches,branch-misses,l1d-tlb-misses,l1i-tlb-misses,l2-tlb-misses-data,l1d-cache-misses,l1i-cache-misses,llc-misses,task-clock,page-faults,duration_time,user_time,system_time,cycles,instructions -- true
  Too many events (max 16)
  [1]

  $ mperf-stat -e cycles,instructions,branches,branch-misses,l1d-tlb-misses,l1i-tlb-misses -e l2-tlb-misses-data,l1d-cache-misses,l1i-cache-misses,llc-misses,task-clock -e page-faults,duration_time,user_time,system_time,cycles,instructions -- true
  Too many events (max 16)
  [1]
