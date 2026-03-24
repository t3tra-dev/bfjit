# Brainf_ck JIT Compiler

This is a JIT compiler for the Brainf\_ck programming language, implemented in C using LLVM. It takes a Brainf\_ck program as input and compiles it to machine code at runtime, allowing for fast execution of Brainf\_ck programs.

## Benchmarks

The benchmark results comparing it to unoptimized Brainf\_ck implementations written in JavaScript and C are as follows:

<!-- benchmark:start -->
```text
Benchmark 1: ./build/bfjit ./examples/hanoi.b.txt
  Time (mean ± σ):      1.003 s ±  0.008 s    [User: 0.965 s, System: 0.038 s]
  Range (min … max):    0.995 s …  1.023 s    10 runs
 
Benchmark 2: ./build/simple_c ./examples/hanoi.b.txt
  Time (mean ± σ):     12.927 s ±  0.663 s    [User: 12.925 s, System: 0.001 s]
  Range (min … max):   11.605 s … 13.444 s    10 runs
 
Benchmark 3: bun ./benchmark/bf.js ./examples/hanoi.b.txt
  Time (mean ± σ):     25.433 s ±  0.541 s    [User: 25.442 s, System: 0.027 s]
  Range (min … max):   24.406 s … 25.725 s    10 runs
 
Summary
  ./build/bfjit ./examples/hanoi.b.txt ran
   12.89 ± 0.67 times faster than ./build/simple_c ./examples/hanoi.b.txt
   25.35 ± 0.58 times faster than bun ./benchmark/bf.js ./examples/hanoi.b.txt
```
<!-- benchmark:end -->

## License

This project is licensed under the MIT License.
