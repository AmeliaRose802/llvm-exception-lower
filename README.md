# llvm-exception-lower

A standalone LLVM pass that lowers C++ exception-handling constructs into
explicit error-flag control flow. The resulting bitcode can be consumed by
downstream tools (symbolic execution engines, verifiers, fuzzers) that do
not model stack unwinding — for example, [SAW](https://github.com/GaloisInc/saw-script).

The pass handles both major C++ exception-handling ABIs:

* The Itanium exception-handling ABI used on Linux and macOS (the
  `invoke` / `landingpad` / `resume` / `__cxa_*` family of constructs).
* The Windows SEH funclet model used by MSVC and `clang-cl` (the
  `catchswitch` / `catchpad` / `cleanuppad` / `catchret` / `cleanupret`
  family).

## Building

Requires LLVM 14 or later and CMake ≥ 3.16.

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

If LLVM is installed in a non-standard location, point CMake at it:

```bash
cmake .. -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
```

## Usage

```bash
# Lower exception constructs in a bitcode file:
./exception-lower input.bc -o output.bc

# The tool also accepts LLVM text IR (.ll) as input.
./exception-lower input.ll -o output.bc
```

## Transformation summary

The pass replaces the following constructs:

| Original construct         | Replacement                                              |
|----------------------------|----------------------------------------------------------|
| `__cxa_allocate_exception` | `alloca` of the requested size                           |
| `__cxa_free_exception`     | Removed (`alloca` self-frees)                            |
| `__cxa_throw`              | Store error type / value to thread-local; return sentinel|
| `__cxa_rethrow`            | Re-set in-flight flag; return sentinel                   |
| `invoke`                   | `call` + load error flag + conditional branch            |
| `landingpad`               | Build `{ ptr, i32 }` from thread-local typeinfo          |
| `__cxa_begin_catch`        | Load thrown value; clear in-flight flag                  |
| `__cxa_end_catch`          | Removed                                                  |
| `resume`                   | Set in-flight flag; return sentinel                      |
| `catchret`                 | Clear in-flight flag; unconditional branch to successor  |
| `cleanupret`               | Branch to unwind destination, or return sentinel         |
| `catchpad`                 | Load thrown value; clear in-flight flag                  |
| `cleanuppad`               | Removed (cleanup body keeps the flag set)                |
| `catchswitch`              | Branch to first handler / unwind / return sentinel       |

## Testing

Compile any C++ source to bitcode, run the pass, and disassemble to
inspect the result:

```bash
clang++ -emit-llvm -c -O0 my-test.cpp -o my-test.bc
./exception-lower my-test.bc -o my-test-lowered.bc
llvm-dis my-test-lowered.bc -o - | less
```

A SAW integration test that exercises the post-lowering shape lives in
the [SAW repository](https://github.com/GaloisInc/saw-script) under
`intTests/test_exception_lower/`.

## License

BSD-3-Clause. See [LICENSE](LICENSE).
