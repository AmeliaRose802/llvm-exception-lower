# llvm-exception-lower

A standalone LLVM pass that lowers C++ exception-handling constructs into
explicit error-flag control flow. The resulting bitcode can be consumed by
downstream tools (symbolic execution engines, verifiers, fuzzers) that do
not model stack unwinding — for example, [SAW](https://github.com/GaloisInc/saw-script).

This pass is primarily used by
[saw-spec-gen](https://github.com/AmeliaRose802/saw-spec-gen), which invokes
it to make exception-handling code tractable for SAW before generating and
discharging specifications.

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

### Lowering modes

The `--mode` flag selects how each function is lowered:

```bash
./exception-lower input.bc --mode=auto         # default
./exception-lower input.bc --mode=full
./exception-lower input.bc --mode=cleanup-only
```

* `auto` (default) — inspect each function and pick the right path: the
  lightweight cleanup-only path (below) for functions that contain only
  cleanup funclets (`cleanuppad` / `cleanupret`) with no catch handlers,
  and the full error-state lowering for everything else.
* `full` — always run the full error-flag lowering, even for
  cleanup-only functions. Useful for forcing uniform output.
* `cleanup-only` — treat *every* function as cleanup-only: demote all
  `invoke`s to ordinary `call`s, drop unwind edges, strip funclets and
  the personality, and delete the now-unreachable EH blocks. No
  error-state globals are created. Catch handlers, if present, are
  conservatively discarded.

### Cleanup-only path (Rust Win64 drop glue)

Rust's MSVC-target drop glue (`core::ptr::drop_in_place::<T>`) emits SEH
funclets purely to run destructors during unwind — there are no catch
handlers and no thrown values to track. For these functions the full
error-state machinery is unnecessary: the cleanup-only path simply
removes the unwind edges and funclet scaffolding, leaving the
destructor calls on the normal return path. The result needs no
`@__exclow_*` globals, which keeps the lowered module trivial for
downstream tools to consume.

`auto` mode detects these functions automatically; a function qualifies
when it contains `cleanuppad` / `cleanupret` and *no*
`catchpad` / `catchswitch` / `catchret` / `landingpad` / `resume`.

## Transformation summary

The pass introduces three module-level `internal global` slots — an in-flight
flag (`@__exclow_error_flag`, `i1`), the thrown `type_info` pointer
(`@__exclow_error_typeinfo`, `ptr`), and the thrown value pointer
(`@__exclow_error_value`, `ptr`) — and rewrites the following constructs in
terms of those slots:

| Original construct         | Replacement                                                      |
|----------------------------|------------------------------------------------------------------|
| `__cxa_allocate_exception` | `alloca` of the requested size                                   |
| `__cxa_free_exception`     | Removed (`alloca` self-frees)                                    |
| `__cxa_throw`              | Store error type / value to flag slots; return sentinel          |
| `__cxa_rethrow`            | Re-set in-flight flag; return sentinel                           |
| `_CxxThrowException`       | (MSVC) Store error type / value to flag slots; return sentinel   |
| `invoke`                   | `call` + load error flag + conditional branch                    |
| `landingpad`               | Build `{ ptr, i32 }` from the typeinfo slot                      |
| `__cxa_begin_catch`        | Load thrown value; clear in-flight flag                          |
| `__cxa_end_catch`          | Removed                                                          |
| `resume`                   | Set in-flight flag; return sentinel                              |
| `catchret`                 | Clear in-flight flag; unconditional branch to successor          |
| `cleanupret`               | Branch to unwind destination, or return sentinel                 |
| `catchpad`                 | Clear in-flight flag                                             |
| `cleanuppad`               | Removed (cleanup body keeps the flag set)                        |
| `catchswitch`              | Typed dispatch chain (`icmp eq` on each handler's type filter)   |
| `[ "funclet"(...) ]` bundle| Stripped from every call / invoke (dead after funclet lowering)  |
| `personality` attribute    | Cleared from every function the pass touches                     |

### Notes for MSVC C++ EH

* The three error-state slots are module-level `internal global`s rather
  than per-function `alloca`s so that exception state propagates across
  call boundaries: when a callee throws and returns a sentinel, the
  caller's `.ehcheck` block reads the same flag. Downstream tools that
  require explicit module-global allocation (notably SAW's crucible-llvm)
  must emit `llvm_alloc_global` for `@__exclow_error_flag`,
  `@__exclow_error_typeinfo`, and `@__exclow_error_value`. The
  [`saw-spec-gen`](https://github.com/AmeliaRose802/saw-spec-gen) tool does
  this automatically via `inject_exclow_globals`.
* `_CxxThrowException(value, throw-info)` is rewritten in the same shape
  as `__cxa_throw` — the throw-info pointer goes into the typeinfo slot,
  the value pointer into the value slot, the in-flight flag is set, and
  trailing `unreachable` (if any) is replaced with a sentinel `ret`.
* `catchswitch` lowering emits a typed dispatch chain comparing the
  in-flight typeinfo against each handler's catchpad type descriptor in
  source order. A catchpad with a null type descriptor (`catch (...)`)
  becomes an unconditional branch and shadows any later handlers. If no
  handler matches, control flows to the catchswitch's unwind destination
  (or to a synthesized sentinel-return block if it has none). Itanium
  landingpad dispatch is lowered the same way: `@llvm.eh.typeid.for`
  calls are eliminated and replaced with direct `icmp eq` against the
  stored typeinfo pointer.
* `"funclet"` operand bundles on calls that used to live inside a
  catchpad / cleanuppad are stripped after funclet lowering. The parent
  token they referenced has been replaced with `undef`, and the bundle
  is rejected by SAW's `llvm-pretty-bc-parser`.
* The MSVC `__CxxFrameHandler3` personality reference is cleared from
  every function the pass touches, so the lowered module is self-
  contained and does not require modelling the personality function.

## Testing

The [tests/](tests/) directory contains a small fixture suite plus a
PowerShell runner. Each fixture embeds its expected post-lowering shape
as `CHECK:` / `CHECK-LABEL:` / `CHECK-NOT:` / `CHECK-DAG:` directives in
trailing comments. Two fixture formats are supported:

* `*.cpp` — a C++ translation unit (compiled to bitcode first), with
  `//`-prefixed CHECK directives.
* `*.ll` — pre-built LLVM text IR (fed straight to the tool, so no
  compiler is required), with `;`-prefixed CHECK directives. The Rust
  drop-glue / cleanup-only fixtures use this form so they remain
  portable across LLVM versions.

A fixture may also declare extra tool arguments via an
`EXCLOW-ARGS:` directive, e.g. `; EXCLOW-ARGS: --mode=cleanup-only`.

Run the whole suite from the repo root:

```powershell
pwsh tests/run.ps1
```

On Linux (or any POSIX shell) use the bash companion:

```bash
./tests/run.sh
```

Both runners share the same fixtures and apply the same embedded
CHECK directives. Verified against LLVM 20 (Windows binary release)
and LLVM 18 (Ubuntu 24.04 `llvm-18-dev` / `clang-18`).

The runner, for each fixture:

1. For `*.cpp`, compiles the source to LLVM bitcode (Windows-MSVC EH by
   default, `x86_64-pc-linux-gnu` Itanium EH for fixtures named
   `itanium_*`); for `*.ll`, uses the IR directly.
2. Runs `exception-lower.exe` on the bitcode, forwarding any
   `EXCLOW-ARGS:` the fixture declares.
3. Disassembles the lowered bitcode to text IR.
4. Runs `opt -passes=verify` on the lowered bitcode.
5. Verifies the lowered text IR satisfies the embedded CHECK directives.

The `CHECK` syntax is a deliberate subset of LLVM's FileCheck — literal
substring matching only, no `{{...}}` regex captures and no strict
order enforcement (because FileCheck itself is not bundled in the
upstream LLVM Windows binary release).

Filter to a single fixture by basename glob:

```powershell
pwsh tests/run.ps1 -Filter add_one_multi*
```

```bash
./tests/run.sh 'add_one_multi*'
```

Tool locations are auto-detected; override with `-LlvmBin` / `-ExcLow` /
`$env:LLVM_BIN` / `$env:EXCLOW_BIN` (PowerShell) or `CLANGXX` / `OPT` /
`LLVM_DIS` / `EXCLOW_BIN` env vars (bash).

For a quick one-off run against an arbitrary source file:

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
