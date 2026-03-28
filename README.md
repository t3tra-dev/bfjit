# Brainf_ck JIT Compiler

This is a JIT compiler for the Brainf\_ck programming language, implemented in C using LLVM. It takes a Brainf\_ck program as input and compiles it to machine code at runtime, allowing for fast execution of Brainf\_ck programs.

## Benchmarks

The benchmark results comparing it to unoptimized Brainf\_ck implementations written in JavaScript and C are as follows:

<!-- benchmark:start -->
```text
Benchmark 1: ./build/bfjit ./examples/hanoi.b.txt
  Time (mean ± σ):       9.1 ms ±   0.3 ms    [User: 8.2 ms, System: 0.8 ms]
  Range (min … max):     8.9 ms …   9.8 ms    10 runs
 
Benchmark 2: ./build/simple_c ./examples/hanoi.b.txt
  Time (mean ± σ):      8.696 s ±  0.153 s    [User: 8.695 s, System: 0.001 s]
  Range (min … max):    8.628 s …  9.104 s    10 runs
 
Benchmark 3: bun ./benchmark/bf.js ./examples/hanoi.b.txt
  Time (mean ± σ):     23.887 s ±  0.027 s    [User: 23.904 s, System: 0.017 s]
  Range (min … max):   23.857 s … 23.943 s    10 runs
 
Summary
  ./build/bfjit ./examples/hanoi.b.txt ran
  956.16 ± 33.50 times faster than ./build/simple_c ./examples/hanoi.b.txt
 2626.35 ± 79.54 times faster than bun ./benchmark/bf.js ./examples/hanoi.b.txt
```
<!-- benchmark:end -->

## License

This project is licensed under the MIT License.
