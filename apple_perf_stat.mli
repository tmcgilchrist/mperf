(** Apple Performance Counters interface for macOS/Apple Silicon

    This module provides a wrapper around the apple-perf-stat CLI tool,
    giving OCaml programs access to hardware performance counters on
    Apple Silicon Macs.

    The tool uses PET (Profile Every Thread) to accurately measure
    multi-threaded programs, including OCaml 5.x programs with multiple
    domains.

    {b Requirements:}
    - macOS with Apple Silicon (M1/M2/M3/M4)
    - The apple-perf-stat binary installed
    - Root privileges (sudo)

    {b Example:}
    {[
      let () =
        match Apple_perf_stat.run
          ~events:["cycles"; "instructions"; "l1d-tlb-misses"]
          ~sample_period_ms:1.0
          ["./benchmark"; "--iterations"; "1000"]
        with
        | Ok m ->
          Printf.printf "IPC: %.2f (%d threads)\n"
            (Apple_perf_stat.get_ipc m)
            m.threads_measured;
          Printf.printf "Wall time: %.3f s\n"
            (Apple_perf_stat.wall_time_seconds m)
        | Error e ->
          Printf.eprintf "Error: %s\n" (Apple_perf_stat.string_of_error e)
    ]}
*)

(** Counter values as (name, value) pairs *)
type counters = (string * int64) list

(** Timing information in nanoseconds *)
type time_info = {
  wall_ns : float;  (** Wall clock time *)
  user_ns : float;  (** User CPU time *)
  sys_ns : float;   (** System CPU time *)
}

(** Derived metrics computed from counter values *)
type derived_metrics = {
  ipc : float option;  (** Instructions per cycle *)
  cpi : float option;  (** Cycles per instruction *)
}

(** Result of a PMC measurement run *)
type measurement = {
  counters : counters;        (** Raw counter values (aggregated across all threads) *)
  time : time_info;           (** Timing information *)
  derived : derived_metrics;  (** Computed metrics *)
  threads_measured : int;     (** Number of threads that were sampled *)
  exit_code : int;            (** Exit code of measured command *)
}

(** Possible errors *)
type error =
  | Not_root              (** Not running as root *)
  | Tool_not_found        (** apple-perf-stat binary not found *)
  | Parse_error of string (** Failed to parse tool output *)
  | Command_failed of int (** Measured command failed *)
  | No_samples of string  (** No thread samples collected (program too short?) *)
  | Unknown of string     (** Other error *)

(** {1 Configuration} *)

(** Set the path to the apple-perf-stat binary.
    Default is [/usr/local/bin/apple-perf-stat] *)
val set_tool_path : string -> unit

(** {1 Running measurements} *)

(** Run a command with PMC measurement using PET (Profile Every Thread).

    All threads in the target process are tracked and their counter
    values are aggregated into the result.

    @param events List of events to measure. Defaults to ["cycles"; "instructions"].
                  See {!section:events} for available events.
    @param sample_period_ms Sampling period in milliseconds. Default is 1.0ms.
                           Shorter periods catch short-lived threads but have more overhead.
    @param command The command and arguments to run.
    @return [Ok result] on success, [Error e] on failure.

    {b Note:} Requires root privileges.

    {b Multi-threading:} For OCaml 5.x programs with N domains, expect to see
    approximately 2*N threads measured (each domain has 2 pthreads). *)
val run : ?events:string list -> ?sample_period_ms:float -> string list -> (measurement, error) result

(** {1 Accessing results} *)

(** Get a specific counter value by name *)
val get_counter : string -> measurement -> int64 option

(** Get instructions per cycle (computed if not in derived metrics) *)
val get_ipc : measurement -> float

(** Get wall clock time in seconds *)
val wall_time_seconds : measurement -> float

(** {1 Output} *)

(** Pretty-print a result *)
val pp_result : Format.formatter -> measurement -> unit

(** Convert an error to a human-readable string *)
val string_of_error : error -> string

(** {1:events Available Events}

    {2 Built-in Aliases}

    These work across different Apple Silicon generations:

    - [cycles] - CPU cycles
    - [instructions] - Retired instructions
    - [branches] - Branch instructions
    - [branch-misses] - Mispredicted branches
    - [l1d-tlb-misses] - L1 data TLB misses
    - [l1i-tlb-misses] - L1 instruction TLB misses
    - [l2-tlb-misses-data] - L2 TLB misses (data)
    - [l1d-cache-misses] - L1 data cache misses
    - [l1i-cache-misses] - L1 instruction cache misses
    - [map-stalls] - Map unit stall cycles
    - [dispatch-stalls] - Dispatch stall cycles

    {2 Raw Event Names}

    You can also use raw event names from the PMC database.
    Run [apple-perf-stat --list] to see all available events
    for your specific CPU.

    The event database files are located at:
    - M1: [/usr/share/kpep/a14.plist]
    - M2: [/usr/share/kpep/a15.plist]
    - M3: [/usr/share/kpep/as1.plist] or [as3.plist]
    - M4: [/usr/share/kpep/as4.plist]

    {1:threading Threading Model}

    The tool uses PET (Profile Every Thread) to sample all threads:

    - A kernel timer fires every [sample_period_ms] milliseconds
    - On each tick, PMC values are recorded for all threads matching the target PID
    - The tool computes (last_sample - first_sample) for each thread
    - Results are summed across all threads

    For OCaml 5.x programs:
    - Each domain has 2 pthreads (domain thread + backup for systhreads)
    - A 4-domain program will show ~8 threads measured
    - Counter values represent total work across all domains
*)