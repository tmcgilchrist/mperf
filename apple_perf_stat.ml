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

(* JSON parsing - minimal implementation without external deps *)
module Json_parse = struct
  (* Very simple JSON parser for our specific output format *)
  
  let trim s = String.trim s
  
  let parse_number s =
    let s = trim s in
    try Some (Float.of_string s)
    with _ -> 
      try Some (Int64.to_float (Int64.of_string s))
      with _ -> None
  
  let parse_int64 s =
    let s = trim s in
    try Some (Int64.of_string s)
    with _ ->
      try Some (Int64.of_float (Float.of_string s))
      with _ -> None
  
  (* Extract value for a key from JSON-like text *)
  let find_value key text =
    let pattern = Printf.sprintf "\"%s\":" key in
    try
      let start = Str.search_forward (Str.regexp_string pattern) text 0 in
      let after_key = start + String.length pattern in
      (* Find the value - either a number or nested object *)
      let rest = String.sub text after_key (String.length text - after_key) in
      let rest = trim rest in
      (* Find end of value (comma, }, or end of string) *)
      let end_pos = 
        try min 
          (try Str.search_forward (Str.regexp "[,}]") rest 0 with Not_found -> max_int)
          (String.length rest)
        with _ -> String.length rest
      in
      Some (trim (String.sub rest 0 end_pos))
    with Not_found -> None
  
  (* Extract all key-value pairs from a section *)
  let extract_section section_name text =
    let pattern = Printf.sprintf "\"%s\":" section_name in
    try
      let start = Str.search_forward (Str.regexp_string pattern) text 0 in
      let after_key = start + String.length pattern in
      let rest = String.sub text after_key (String.length text - after_key) in
      (* Find matching braces *)
      let brace_start = String.index rest '{' in
      let rec find_end pos depth =
        if pos >= String.length rest then pos
        else match rest.[pos] with
          | '{' -> find_end (pos + 1) (depth + 1)
          | '}' -> if depth = 1 then pos + 1 else find_end (pos + 1) (depth - 1)
          | _ -> find_end (pos + 1) depth
      in
      let brace_end = find_end brace_start 0 in
      Some (String.sub rest brace_start (brace_end - brace_start))
    with _ -> None
  
  (* Parse counter values from counters section *)
  let parse_counters text =
    match extract_section "counters" text with
    | None -> []
    | Some section ->
      (* Match "key": value patterns *)
      let re = Str.regexp "\"\\([^\"]+\\)\"[ \t\n]*:[ \t\n]*\\([0-9]+\\)" in
      let rec find_all pos acc =
        try
          let _ = Str.search_forward re section pos in
          let key = Str.matched_group 1 section in
          let value = Str.matched_group 2 section in
          let next = Str.match_end () in
          match parse_int64 value with
          | Some v -> find_all next ((key, v) :: acc)
          | None -> find_all next acc
        with Not_found -> List.rev acc
      in
      find_all 0 []
  
  (* Parse time section *)
  let parse_time text =
    let get_float key default =
      match extract_section "time" text with
      | None -> default
      | Some section ->
        match find_value key section with
        | Some v -> (match parse_number v with Some f -> f | None -> default)
        | None -> default
    in
    {
      wall_ns = get_float "wall_ns" 0.0;
      user_ns = get_float "user_ns" 0.0;
      sys_ns = get_float "sys_ns" 0.0;
    }
  
  (* Parse derived metrics *)
  let parse_derived text =
    let get_float key =
      match extract_section "derived" text with
      | None -> None
      | Some section ->
        match find_value key section with
        | Some v -> parse_number v
        | None -> None
    in
    {
      ipc = get_float "ipc";
      cpi = get_float "cpi";
    }
end

let parse_json_output output =
  let counters = Json_parse.parse_counters output in
  let time = Json_parse.parse_time output in
  let derived = Json_parse.parse_derived output in
  let threads_measured = 
    match Json_parse.find_value "threads_measured" output with
    | Some v -> (match Json_parse.parse_int64 v with 
                 | Some n -> Int64.to_int n 
                 | None -> 0)
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
  Format.fprintf fmt "@]"

let string_of_error = function
  | Not_root -> "Root privileges required (run with sudo)"
  | Tool_not_found -> Printf.sprintf "Tool not found at %s" !tool_path
  | Parse_error msg -> Printf.sprintf "Failed to parse output: %s" msg
  | Command_failed code -> Printf.sprintf "Command failed with exit code %d" code
  | No_samples msg -> Printf.sprintf "No samples: %s" msg
  | Unknown msg -> Printf.sprintf "Unknown error: %s" msg