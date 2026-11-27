(lang dune 3.0)
(name macperf)
(version 0.1.0)

(generate_opam_files true)

(source (github tmcgilchrist/mperf))
(authors "Author Name")
(maintainers "maintainer@example.com")
(license MIT)

(package
 (name macperf)
 (synopsis "OCaml bindings for macOS hardware performance counters")
 (description "Access hardware performance counters (PMC) on macOS (Apple Silicon and Intel).
Provides IPC, cache misses, TLB misses, branch mispredictions, and more.")
 (depends
  (ocaml (>= 4.14))
  dune))
