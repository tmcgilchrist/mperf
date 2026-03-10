(lang dune 3.0)
(name apple-perf-stat)
(version 0.1.0)

(generate_opam_files true)

(source (github username/apple-perf-stat))
(authors "Author Name")
(maintainers "maintainer@example.com")
(license MIT)

(package
 (name apple-perf-stat)
 (synopsis "OCaml bindings for Apple Silicon performance counters")
 (description "Access hardware performance counters (PMC) on Apple Silicon Macs. 
Provides IPC, cache misses, TLB misses, branch mispredictions, and more.")
 (depends
  (ocaml (>= 4.14))
  dune))
