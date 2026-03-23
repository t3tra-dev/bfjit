# Brainf_ck JIT Compiler

This is a JIT compiler for the Brainf\_ck programming language, implemented in C using LLVM. It takes a Brainf\_ck program as input and compiles it to machine code at runtime, allowing for fast execution of Brainf\_ck programs.

## Benchmarks

The benchmark results comparing it to unoptimized Brainf\_ck implementations written in JavaScript and C are as follows:

<!-- benchmark:start -->
```text
Benchmark 1: ./build/bfjit ./examples/hanoi.b.txt
  Time (mean ± σ):     147.6 ms ±   5.2 ms    [User: 140.1 ms, System: 5.5 ms]
  Range (min … max):   144.3 ms … 161.8 ms    10 runs
 
Benchmark 2: ./build/simple_c ./examples/hanoi.b.txt
  Time (mean ± σ):      7.497 s ±  0.108 s    [User: 7.447 s, System: 0.033 s]
  Range (min … max):    7.382 s …  7.660 s    10 runs
 
Benchmark 3: bun ./benchmark/bf.js ./examples/hanoi.b.txt
  Time (mean ± σ):     12.688 s ±  0.304 s    [User: 12.608 s, System: 0.065 s]
  Range (min … max):   12.110 s … 13.021 s    10 runs
 
Summary
  ./build/bfjit ./examples/hanoi.b.txt ran
   50.80 ± 1.94 times faster than ./build/simple_c ./examples/hanoi.b.txt
   85.99 ± 3.68 times faster than bun ./benchmark/bf.js ./examples/hanoi.b.txt
```
<!-- benchmark:end -->

## License

This project is licensed under the MIT License.
