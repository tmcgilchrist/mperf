The usage text goes to stderr and names the binary as it was invoked. --help
and -h print it and exit 0.

  $ mperf --help
  Usage: sudo mperf [options] -- command [args...]
         sudo mperf [options] -p PID
  
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
    -i, --no-inherit     Count only the process itself, not its children
                         (always so; --inherit is accepted with a warning)
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
    sudo mperf -e cycles -e instructions -- ./my_benchmark
    sudo mperf -j -e cycles -e l1d-tlb-misses -- ./ocaml_program
  

  $ mperf --help > long 2>&1
  $ mperf -h > short 2>&1
  $ cmp long short

With no command to measure there is nothing to do: report it, show the usage,
and fail.

  $ mperf
  Error: No command specified
  Usage: sudo mperf [options] -- command [args...]
         sudo mperf [options] -p PID
  
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
    -i, --no-inherit     Count only the process itself, not its children
                         (always so; --inherit is accepted with a warning)
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
    sudo mperf -e cycles -e instructions -- ./my_benchmark
    sudo mperf -j -e cycles -e l1d-tlb-misses -- ./ocaml_program
  
  [1]

  $ mperf -e cycles
  Error: No command specified
  Usage: sudo mperf [options] -- command [args...]
         sudo mperf [options] -p PID
  
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
    -i, --no-inherit     Count only the process itself, not its children
                         (always so; --inherit is accepted with a warning)
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
    sudo mperf -e cycles -e instructions -- ./my_benchmark
    sudo mperf -j -e cycles -e l1d-tlb-misses -- ./ocaml_program
  
  [1]
