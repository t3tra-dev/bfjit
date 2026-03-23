# Brainf_ck JIT Compiler

This is a JIT compiler for the Brainf\_ck programming language, implemented in C using LLVM. It takes a Brainf\_ck program as input and compiles it to machine code at runtime, allowing for fast execution of Brainf\_ck programs.

## Benchmarks

The benchmark results comparing it to unoptimized Brainf\_ck implementations written in JavaScript and C are as follows:

<!-- benchmark:start -->
```text
Benchmark 1: ./build/bfjit ./examples/hanoi.b.txt
  Time (mean ± σ):     408.6 ms ±   2.1 ms    [User: 388.3 ms, System: 20.0 ms]
  Range (min … max):   406.2 ms … 413.3 ms    10 runs
 
Benchmark 2: ./build/simple_c ./examples/hanoi.b.txt
  Time (mean ± σ):     12.725 s ±  0.744 s    [User: 12.722 s, System: 0.001 s]
  Range (min … max):   11.546 s … 13.447 s    10 runs
 
Benchmark 3: bun ./benchmark/bf.js ./examples/hanoi.b.txt
  Time (mean ± σ):     25.439 s ±  0.544 s    [User: 25.448 s, System: 0.028 s]
  Range (min … max):   24.405 s … 25.716 s    10 runs
 
Summary
  ./build/bfjit ./examples/hanoi.b.txt ran
   31.14 ± 1.83 times faster than ./build/simple_c ./examples/hanoi.b.txt
   62.26 ± 1.37 times faster than bun ./benchmark/bf.js ./examples/hanoi.b.txt
```
<!-- benchmark:end -->

## License

This project is licensed under the MIT License.
