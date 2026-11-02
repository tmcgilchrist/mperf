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
   event, with no enclosing array and no footer. Each carries "event" and
   "counter-value"; the timings arrive as perf's tool events, which [run]
   always asks for. *)
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
end

(* perf's tool events for the run's own times, in ns *)
let time_events = ["duration_time"; "user_time"; "system_time"]

let parse_json_output output =
  let events =
    List.filter_map
      (fun line ->
         match
           Json_parse.find_field "event" line,
           Json_parse.float_field "counter-value" line
         with
         | Some name, Some value ->
           (* task-clock is the one event perf reports in msec *)
           let scale =
             if Json_parse.find_field "unit" line = Some "msec" then 1e6 else 1.0
           in
           Some (name, value *. scale)
         | _ -> None)
      (Json_parse.lines output)
  in

  let ns name = Option.value (List.assoc_opt name events) ~default:0.0 in
  let time = {
    wall_ns = ns "duration_time";
    user_ns = ns "user_time";
    sys_ns = ns "system_time";
  } in

  let counters =
    events
    |> List.filter (fun (name, _) -> not (List.mem name time_events))
    |> List.map (fun (name, value) -> (name, Int64.of_float value))
  in

  (* perf 7.0 prints no metrics for events named with -e, so derive them. *)
  let count name =
    match List.assoc_opt name counters with
    | Some v when v > 0L -> Some (Int64.to_float v)
    | _ -> None
  in
  let cpu_ns = time.user_ns +. time.sys_ns in
  let ipc =
    match count "cycles", count "instructions" with
    | Some c, Some i -> Some (i /. c)
    | _ -> None
  in
  let derived = {
    ipc;
    cpi = Option.map (fun i -> 1.0 /. i) ipc;
    ghz =
      (match count "cycles" with
       | Some c when cpu_ns > 0.0 -> Some (c /. cpu_ns)
       | _ -> None);
  } in

  { counters; time; derived; exit_code = 0 }

let run ?(events = ["cycles"; "instructions"]) ?(sample_period_ms = 1.0) command =
  (* Check if we're root *)
  if Unix.geteuid () <> 0 then
    Error Not_root
  else if not (Sys.file_exists !tool_path) then
    Error Tool_not_found
  else begin
    (* Build command line *)
    let event_args =
      events @ List.filter (fun e -> not (List.mem e events)) time_events
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
        if List.for_all (fun (_, v) -> v = 0L) result.counters then
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
  Format.fprintf fmt "Counters:@.";
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