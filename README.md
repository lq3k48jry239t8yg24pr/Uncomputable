# Uncomputable

This repository is an experimental, research-oriented exploration of small Binary Lambda Calculus (BLC) programs, expressed as a concrete database of enumerated proofs and outputs. It is not intended as a polished user-facing application.

## What this repository contains

- `blc_full.cpp`
  - Main generator/evaluator for small BLC programs.
  - Enumerates programs by binary length and evaluates them in parallel.
  - Writes compact binary dump files into `blc_dumps/`.
  - Records whether a program hits the step limit or halts, and captures output bits.
  - Includes checkpoint/resume support via `blc_dumps/checkpoint.txt`.

- `blc_reader.cpp`
  - Reader/inspector for the generated dump files.
  - Prints frequency information for outputs and counts of step-limit hits.

- `blc_theorem_extractor.cpp`
  - Reads dump files and attempts to infer types for each parsed BLC term.
  - Converts successful type inferences into theorem-like propositions.
  - Writes extracted theorems into `theorems/<len>.txt`.

- `terminal_blc_theorem_extractor.cpp`
  - Interactive terminal variant of the theorem extractor.
  - Prints discovered theorems and witness outputs directly to the console.

- `kolmogorov.cpp`
  - Scans generated dumps to identify the shortest program found for each distinct output.
  - Writes CSV summaries to `results/kolmogorov.csv`.

- `solomonoff.cpp`
  - Approximates a Solomonoff-like prior over outputs from the generated data.
  - Writes text summaries to `results/solomonoff.txt`.

- `compress_all.cpp`
  - Utility to gzip-compress files in `blc_dumps/` and `theorems/`. Not used anymore due to main files already utilizing gzip-compression natively.

- `compression_helpers.h`, `file_splitter.h`
  - Helpers for transparent decompression, gzip handling, and split-file merging.

## Results files

- `results/kolmogorov.csv`
  - CSV table with one row per unique output bitstring discovered in the dump files.
  - Columns:
    - `output`: the raw output bitstring (empty outputs are represented as `<empty>` in the generated CSV).
    - `complexity`: the shortest program length that produced that output. Some of these are exact due to there being no non-halting programs before it.
    - `example_program`: a witness BLC program of that minimal length.

- `results/solomonoff.txt`
  - Text log of an approximate Solomonoff prior over outputs, built from the enumerated halting programs.
  - Includes per-length dump-file summaries and cumulative log2 prior mass.
  - Each section reports:
    - records processed for that length,
    - halting vs non-halting counts,
    - log2 of cumulative prior mass,
    - per-output contributions and cumulative prior updates.

## Data and encoding

The project stores program evaluation results in bit-packed files under `blc_dumps/`.

Each record in a dump file is encoded as:

1. `len` bits: the BLC program itself.
2. status bit:
   - `1` = the program hit the step limit / did not produce a halting output.
   - `0` = the program halted and produced an output.
3. if halting (`0`): 32-bit big-endian output length, followed by `output_length` bits of output.

The BLC program format is standard binary lambda calculus with de Bruijn indices:

- `00` = lambda abstraction
- `01` = application
- `1^n 0` = variable with de Bruijn index `n`

Output values are stored as raw bitstrings.

## Build notes

There is no packaged build system. The code is intended to be compiled directly with a C++17-capable compiler.

## Important caveats

- This repository is a working research artifact, not a consumer library.
- No installation scripts or dependency management are provided.
- The code assumes a Linux-like environment with `gzip` available.
- The output files can be large; generation is not optimized for general use.
- The source is oriented toward experiments in enumerating and analyzing BLC programs.

## Directory structure

- `blc_dumps/` - generated binary dump files and checkpoint metadata.
- `results/` - derived summaries such as `kolmogorov.csv` and `solomonoff.txt`.
- `theorems/` - extracted theorem propositions per program length.

## Philosophy

This repository is more of a concrete, working database than a reusable project. It turns theoretical enumerations of tiny lambda programs into tangible data, records their halting behavior, and then uses that corpus to explore both type-theoretic theorem extraction and algorithmic-probability-inspired summaries.
