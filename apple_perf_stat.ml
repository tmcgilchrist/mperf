(* apple_perf_stat.ml
   
   OCaml wrapper for apple-perf-stat CLI tool.
   Designed for integration with OCaml benchmarking services.
   
   Usage:
     let result = Apple_perf_stat.run 
       ~events:["cycles"; "instructions"; "l1d-tlb-misses"]
       ["./my_benchmark"; "--size"; "1000"]
     in
     Printf.printf "IPC: %.2f\n" result.derived.ipc
*)

type counters = (string * int64) list

type time_info = {
  wall_ns : float;
  user_ns : float;
  sys_ns : float;
}

type derived_metrics = {
  ipc : float option;
  cpi : float option;
  ghz : float option;
}

type measurement = {
  counters : counters;
  time : time_info;
  derived : derived_metrics;
  threads_measured : int;
  exit_code : int;
}

type error =
  | Not_root
  | Tool_not_found
  | Parse_error of string
  | Command_failed of int
  | No_samples of string
  | Unknown of string

let tool_path = ref "/usr/local/bin/apple-perf-stat"

let set_tool_path path = tool_path := path

(* JSON parsing - minimal implementation without external deps.

   The tool emits perf stat --json's newline-delimited JSON: one object per
   line, with no enclosing array. Counter objects carry "event" and
   "counter-value"; the timings and the thread count arrive as metric-only
   objects, because perf writes no footer at all in JSON mode. *)
module Json_parse = struct
  (* Value of [key] within a single NDJSON line. Values are either quoted
     ("counter-value" : "123.000000") or bare ("event-runtime" : 4096); both
     come back as raw text with the quotes stripped. *)
  let find_field key line =
    let re =
      Str.regexp
        ("\"" ^ Str.quote key
         ^ "\"[ \t]*:[ \t]*\\(\"\\([^\"]*\\)\"\\|[-+0-9.eE]+\\)")
    in
    match Str.search_forward re line 0 with
    | exception Not_found -> None
    | _ ->
      (* Group 2 participates only when the quoted branch matched. *)
      (match Str.matched_group 2 line with
       | s -> Some s
       | exception Not_found -> Some (Str.matched_group 1 line))

  let float_field key line =
    match find_field key line with
    | None -> None
    | Some s -> float_of_string_opt (String.trim s)

  let lines text =
    String.split_on_char '\n' text
    |> List.filter (fun l -> String.trim l <> "")

  (* A metric is identified by its unit string, which is the one perf prints in
     its text footer. Counter objects carry their own metric, so this also
     finds "insn per cycle" on the instructions line. *)
  let metric unit lines =
    List.find_map
      (fun line ->
         match find_field "metric-unit" line with
         | Some u when u = unit -> float_field "metric-value" line
         | _ -> None)
      lines

  (* perf names a metric's unit even when it has no value for it, writing
     "metric-value" : "0.000000", and mperf reproduces that. Neither a clock
     speed nor an insn-per-cycle figure can genuinely be zero, so read a zero
     as absent rather than passing it on as a measurement. *)
  let computed_metric unit lines =
    match metric unit lines with
    | Some v when v > 0.0 -> Some v
    | _ -> None
end

let parse_json_output output =
  let lines = Json_parse.lines output in

  let counters =
    List.filter_map
      (fun line ->
         match
           Json_parse.find_field "event" line,
           Json_parse.float_field "counter-value" line
         with
         | Some name, Some value -> Some (name, Int64.of_float value)
         | _ -> None)
      lines
  in

  let seconds unit =
    match Json_parse.metric unit lines with Some s -> s *. 1e9 | None -> 0.0
  in
  let time = {
    wall_ns = seconds "seconds time elapsed";
    user_ns = seconds "seconds user";
    sys_ns = seconds "seconds sys";
  } in

  (* perf reports insn per cycle only, leaving the reciprocal to the caller. *)
  let ipc = Json_parse.computed_metric "insn per cycle" lines in
  let derived = {
    ipc;
    cpi = (match ipc with Some i when i > 0.0 -> Some (1.0 /. i) | _ -> None);
    ghz = Json_parse.computed_metric "GHz" lines;
  } in

  let threads_measured =
    match Json_parse.metric "threads measured" lines with
    | Some t -> int_of_float t
    | None -> 0
  in

  { counters; time; derived; threads_measured; exit_code = 0 }

let run ?(events = ["cycles"; "instructions"]) ?(sample_period_ms = 1.0) command =
  (* Check if we're root *)
  if Unix.geteuid () <> 0 then
    Error Not_root
  else if not (Sys.file_exists !tool_path) then
    Error Tool_not_found
  else begin
    (* Build command line *)
    let event_args = 
      events 
      |> List.map (fun e -> ["-e"; e]) 
      |> List.flatten 
    in
    let period_args = ["-P"; Printf.sprintf "%.1f" sample_period_ms] in

    (* The tool writes its report to stderr, which the measured command also
       inherits, so ask for it in a file rather than trying to demultiplex.
       stdin/stdout/stderr are passed through untouched to the command. *)
    let report_file = Filename.temp_file "mperf" ".json" in
    Fun.protect ~finally:(fun () -> try Sys.remove report_file with Sys_error _ -> ())
    @@ fun () ->

    let args =
      [!tool_path] @ ["-j"] @ ["-o"; report_file] @ period_args @ event_args
      @ ["--"] @ command
      |> Array.of_list
    in

    let pid = Unix.create_process !tool_path args
                Unix.stdin Unix.stdout Unix.stderr in

    (* Wait for process *)
    let _, status = Unix.waitpid [] pid in
    let exit_code = match status with
      | Unix.WEXITED c -> c
      | Unix.WSIGNALED s -> 128 + s
      | Unix.WSTOPPED s -> 128 + s
    in

    let output =
      try In_channel.with_open_bin report_file In_channel.input_all
      with Sys_error _ -> ""
    in

    if exit_code <> 0 && String.length output = 0 then
      Error (Command_failed exit_code)
    else
      try
        let result = parse_json_output output in
        if result.threads_measured = 0 then
          Error (No_samples "No thread samples collected - program may have been too short")
        else
          Ok { result with exit_code }
      with e ->
        Error (Parse_error (Printexc.to_string e))
  end

(* Convenience functions *)

let get_counter name result =
  List.assoc_opt name result.counters

let get_ipc result =
  match result.derived.ipc with
  | Some ipc -> ipc
  | None ->
    match get_counter "cycles" result, get_counter "instructions" result with
    | Some c, Some i when c > 0L -> 
        Int64.to_float i /. Int64.to_float c
    | _ -> 0.0

let wall_time_seconds result =
  result.time.wall_ns /. 1e9

(* Pretty printing *)

let pp_result fmt result =
  Format.fprintf fmt "@[<v>";
  Format.fprintf fmt "Counters (%d thread%s):@." 
    result.threads_measured 
    (if result.threads_measured = 1 then "" else "s");
  List.iter (fun (name, value) ->
    Format.fprintf fmt "  %s: %Ld@." name value
  ) result.counters;
  Format.fprintf fmt "Time:@.";
  Format.fprintf fmt "  wall: %.6f s@." (result.time.wall_ns /. 1e9);
  Format.fprintf fmt "  user: %.6f s@." (result.time.user_ns /. 1e9);
  Format.fprintf fmt "  sys:  %.6f s@." (result.time.sys_ns /. 1e9);
  (match result.derived.ipc with
   | Some ipc -> Format.fprintf fmt "IPC: %.4f@." ipc
   | None -> ());
  (match result.derived.ghz with
   | Some ghz -> Format.fprintf fmt "Clock: %.3f GHz@." ghz
   | None -> ());
  Format.fprintf fmt "@]"

let string_of_error = function
  | Not_root -> "Root privileges required (run with sudo)"
  | Tool_not_found -> Printf.sprintf "Tool not found at %s" !tool_path
  | Parse_error msg -> Printf.sprintf "Failed to parse output: %s" msg
  | Command_failed code -> Printf.sprintf "Command failed with exit code %d" code
  | No_samples msg -> Printf.sprintf "No samples: %s" msg
  | Unknown msg -> Printf.sprintf "Unknown error: %s" msg