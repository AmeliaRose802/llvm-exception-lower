# `saw-tools/exception-lower` — gaps blocking real SAW verification of MSVC C++ EH

**Audience:** SAW team / maintainers of
[`saw-tools/exception-lower`](https://github.com/GaloisInc/saw-script/tree/master/saw-tools/exception-lower).
**Author:** saw-spec-gen integration (Amelia Payne).
**Date:** 2026-05-27.
**TL;DR:** With the pass in its current state, a single-handler MSVC
`try`/`catch` *can* be made to verify against a total Cryptol spec
in SAW, but only after substantial textual post-processing of the
pass output. As soon as the program has more than one catch arm or
the throw site is in the same function as the catch, the pass
either drops the multi-arm dispatch or fails to lower
`_CxxThrowException`, and SAW can no longer distinguish a
"harmless" catch from a "harmful" one.

Three concrete fixes are requested. Each is independently useful;
together they make MSVC C++ EH a first-class target for SAW
verification on Windows.

## Test corpus

Three minimal demos under
[`demo/vtable_havoc_spec_demos/throws_exception/`](demo/vtable_havoc_spec_demos/throws_exception/):

| File | What it exercises | Pre-pass | Post-pass (current) | Post-pass (ideal) |
|------|--------------------|----------|----------------------|---------------------|
| [`add_one_sat.cpp`](add_one_sat.cpp) | no EH at all | SAT (VERIFIED) | — | — |
| [`add_one_throws.cpp`](add_one_throws.cpp) | propagating `throw 1;` (no catch) | UNSAT (`x = 42`) | UNSAT (`x = 42`) — pass is a no-op | same |
| [`add_one_throws_caught.cpp`](add_one_throws_caught.cpp) | single `try` / `catch (int)`, `printf`, fall-through | bitcode parser fails on `FUNC_CODE_CATCHSWITCH` | SAT (VERIFIED) — only after our post-processing script | SAT (VERIFIED) directly |
| [`add_one_multi_catch.cpp`](add_one_multi_catch.cpp) | two catch arms (`HarmlessTag`, `HarmfulTag`); only `HarmfulTag` arm violates spec | bitcode parser fails on `FUNC_CODE_CATCHSWITCH` | UNSAT but with **wrong counterexample** (see §3) | UNSAT (`x = 99`, the harmful arm), or SAT if the harmful arm is removed |

Bitcode produced by `clang++ -target x86_64-pc-windows-msvc -O0`
(LLVM 20.1.6 native Windows). All four demos load cleanly when
`exception-lower` is replaced or bypassed; the gaps below are
specifically about what the pass leaves on the floor when it *is*
invoked.

## Reproducing

Native Windows build of the pass (no WSL needed) succeeds against
the LLVM 20.1.6 standalone distribution after one local patch to
`lib/cmake/llvm/LLVMExports.cmake` — that file hard-codes a
VS2019 path for `diaguids.lib`. Pointing it at VS2022's DIA SDK
lets `cmake -G Ninja` under `vcvars64.bat` produce a working
`exception-lower.exe`. (Likely not a pass bug; flagging because
the upstream distribution probably wants either a `find_package`
search or DIA-less linkage so out-of-the-box Windows builds
"just work".)

The end-to-end pipeline our integration runs:

```text
clang++ ──► .bc
   │
   ▼
saw-spec-gen patch-llvm-ir --strip-msvc-eh --poison-to-undef
   │  (strips _TI*/_CTA*/_CT??_R0* `.xdata` globals that crucible-llvm rejects;
   │   converts `poison` to `undef` to dodge a Crucible aggregate-constant panic)
   ▼
llvm-as → .bc
   │
   ▼
exception-lower in.bc -o lowered.bc        ◄── this document is about this step
   │
   ▼
llvm-dis lowered.bc → lowered.ll
   │
   ▼
fix_exclow_i8.ps1   (see Gap 1 / Gap 2 below — strips funclet bundles,
                     deletes __exclow_* globals, folds the flag check)
   │
   ▼
llvm-as → clean.bc
   │
   ▼
saw  ◄── llvm_load_module, mir_verify
```

The fix-up script is at
[`scripts/fix_exclow_i8.ps1`](../../../scripts/fix_exclow_i8.ps1).
Every transformation it performs is something the pass arguably
should have done itself — the bullets below are extracted
from there.

## Gap 1 — Leftover `[ "funclet"(token undef) ]` operand bundles

**Severity:** P0 — blocks SAW from even loading the lowered module.

After the pass demotes the catchpad/catchswitch structure, calls
that lived inside a catchpad still carry their original funclet
operand bundle:

```llvm
%7 = call i32 (ptr, ...) @printf(ptr noundef @.str)
       [ "funclet"(token undef) ]
```

SAW's `llvm-pretty-bc-parser` has no decoder for
`FUNC_CODE_OPERAND_BUNDLE`, so this aborts module load with:

```
parseField: unable to parse record field …
    FUNC_CODE_OPERAND_BUNDLE
    @"?add_one@@YAII@Z"
```

Once the parent catchpad token has been replaced (typically by
`undef`), the operand bundle is just dead metadata referring to a
token that no longer exists. The bundle annotation can be safely
dropped at the same time the parent funclet structure is dropped.

**Requested fix:** In `ExceptionLowerPass.cpp`, after rewriting a
catchpad/cleanuppad, also visit every `CallInst`/`InvokeInst`
inside that funclet and remove its `"funclet"` operand bundle.
LLVM exposes a helper: clone the call with
`CallBase::removeOperandBundle("funclet")` (or use
`CallBase::Create(..., NoBundles, ...)`). The current
`llvm-pretty-bc-parser` will then accept the output unchanged.

Our workaround (and demonstration that the fix is sufficient) is
in `fix_exclow_i8.ps1` step 3, a single regex deletion of the
bundle text.

## Gap 2 — Internal thread-local globals never get a SAW allocation

**Severity:** P0 — even after Gap 1 is worked around, every load of
`__exclow_error_flag` aborts symbolic execution.

The pass introduces three globals for the "in-flight exception"
state machine:

```llvm
@__exclow_error_flag      = internal thread_local global i1   false
@__exclow_error_typeinfo  = internal thread_local global ptr  null
@__exclow_error_value     = internal thread_local global ptr  null
```

SAW's globals allocator does not allocate any of them. The very
first load (`%exclow.err = load i1, ptr @__exclow_error_flag`)
aborts with:

```
internal: error: in ?add_one@@YAII@Z
  Global symbol not allocated
  Details:
    Global symbol "__exclow_error_flag" has no associated allocation
```

This reproduces with `internal global`, `dso_local global`, plain
`global`, and `i1` vs `i8` element type — every combination we
tried. It is **not** specific to the TLS qualifier (we tried
stripping `thread_local`), nor to `i1`. The pattern that SAW
*does* allocate, as observed in our other demo modules, is
`dso_local global TYPE INIT, align N` *attached to a function that
SAW visits*. The pass-generated globals are referenced only from
the function under verification, so by inspection they should
qualify, yet they don't.

We have not been able to identify what crucible-llvm wants. Our
workaround is to delete the globals entirely *and* fold every
`load i1, ptr @__exclow_error_flag` to a constant
(`add i1 0, 0`). That works for the single-catch demo because
the throws sit behind an override that is assumed not to fault,
but it defeats inline-throw analysis (Gap 3).

**Requested fix (preferred):** Replace the three TLS globals with
function-local `alloca`s threaded through the pass's existing
control-flow rewrite. Locals are always allocated by SAW. They
also remove an extra concern about global-state aliasing that
isn't real (the error machine is "consumed and reset" within a
single try region anyway).

**Requested fix (alternative):** If preserving globals is
important for compositionality, please document the exact
linkage / alignment / section incantation that SAW's
globals allocator recognises. We searched
`crucible-llvm/src/Lang/Crucible/LLVM/Globals.hs` and could not
identify the predicate that filters globals out.

## Gap 3 — `catchswitch` with multiple handlers collapses to "first handler"

**Severity:** P1 — silently changes program semantics; SAW will
return wrong verdicts on multi-catch programs.

The pass's README documents `catchswitch` as

> Branch to first handler / unwind / return sentinel

In practice this means a two-arm `try`/`catch`/`catch` lowers
with both arms pointing at the *first* catch's basic block. We
observed this on
[`add_one_multi_catch.cpp`](add_one_multi_catch.cpp), where:

```cpp
try {
    if (x == 7u)  throw HarmlessTag{};
    if (x == 99u) throw HarmfulTag{};
} catch (const HarmlessTag&) { /* harmless: prints, falls through */ }
catch (const HarmfulTag&)    { return 0;        /* spec-violating */ }
return x + 1;
```

In the lowered IR (excerpt; `lowered.ll` in
`out_add_one_multi_catch/`):

```llvm
.ehcheck:                          ; preds = %8 (HarmlessTag throw)
  %exclow.err = load i1, ptr @__exclow_error_flag
  br i1 %exclow.err, label %13, label %25      ; %13 = HarmlessTag arm

.ehcheck1:                         ; preds = %12 (HarmfulTag throw)
  %exclow.err2 = load i1, ptr @__exclow_error_flag
  br i1 %exclow.err2, label %13, label %25     ; %13 = HarmlessTag arm too!

20:                                ; No predecessors!
  store i32 0, ptr %2, align 4                 ; HarmfulTag's `return 0` — orphaned
```

The `HarmfulTag` arm (the one that should expose a counterexample)
ends up as `; No predecessors!`. The type discriminator
(`@"_TI1?AUHarmlessTag@@"` vs `@"_TI1?AUHarmfulTag@@"`) recorded
at each throw site is not consulted.

**Requested fix:** At lowering time, the pass already sees both
the throw's typeinfo argument (from `_CxxThrowException`'s second
operand or `__cxa_throw`'s second operand) and each catchpad's
type filter. Emit a chain like:

```llvm
.ehcheck:
  %typed = load ptr, ptr @__exclow_error_typeinfo
  %is_h1 = icmp eq ptr %typed, @"_TI1?AUHarmlessTag@@"
  br i1 %is_h1, label %handler_harmless, label %next
next:
  %is_h2 = icmp eq ptr %typed, @"_TI1?AUHarmfulTag@@"
  br i1 %is_h2, label %handler_harmful, label %unhandled
```

…instead of the current single unconditional branch to the
first handler. The harmless/harmful distinction then propagates
to SAW: only the harmful path produces a return that disagrees
with the spec, and Z3 will hand back precisely
`x = 99` as the counterexample.

## Gap 4 — `_CxxThrowException` is not lowered (MSVC throw site is left as-is)

**Severity:** P1 — pairs with Gap 3 to make multi-catch demos
unverifiable.

`__cxa_throw` (Itanium ABI) is rewritten by the pass to "set
in-flight flag + typeinfo + return sentinel". The MSVC
equivalent `_CxxThrowException` is left as an external `declare`.
Looking at `lowered.ll`:

```llvm
8:
  call void @_CxxThrowException(ptr %4, ptr @"_TI1?AUHarmlessTag@@") #4
  br label %.ehcheck         ; .ehcheck reads __exclow_error_flag…

declare dso_local void @_CxxThrowException(ptr, ptr)
```

The follow-on `.ehcheck` block reads `__exclow_error_flag`, but
nothing ever sets it to `true`, so the flag is provably `false`
along the path that did the throw. With Gap 3 also unfixed, both
catch arms branch into the harmless handler, but with Gap 3 fixed,
the typeinfo check would see a null pointer (the flag was never
set), would not match anything, and would fall through to
`unhandled`. Either way, the throw is effectively dead.

When SAW then symbolically executes the `_CxxThrowException`
call (`Failed to load function handle / No implementation or
override found for pointer`), it converts the entire arm into a
"failed assertion" with whatever symbolic input first reaches the
call. In our multi-catch run that input happened to be
`x = 7` (the HarmlessTag throw site, reached first by SBV->Z3);
in a program where the harmless throw is removed and only the
harmful arm exists, the counterexample would be `x = 99`. Either
way the verdict is "the throw didn't return correctly" rather
than "the harmful catch returned the wrong value", which is the
semantic distinction we actually care about.

**Requested fix:** Add an MSVC counterpart to the
`__cxa_throw` rewrite. At every `call @_CxxThrowException(ptr
%value, ptr %typeinfo)`:

```llvm
store i1  true,        ptr @__exclow_error_flag
store ptr %typeinfo,   ptr @__exclow_error_typeinfo
store ptr %value,      ptr @__exclow_error_value
; replace the call with a branch to the function's sentinel-return path,
; or to a fresh basic block ending in `ret <undef sentinel>` if no caller
; ever checks the flag (i.e. throw escapes the function)
```

The typeinfo argument is already exactly the operand the catchpad
type filters compare against, so it round-trips cleanly into the
Gap 3 dispatch chain.

(For completeness: `__CxxFrameHandler3` is the MSVC personality
function. It's only referenced via the `personality` attribute on
the function header and never called at runtime, so it does not
need a rewrite — but the personality attribute can be stripped
along with the other MSVC EH metadata to keep the loaded module
self-contained.)

## Why this matters

The whole point of running C++ verification on Windows is to
catch bugs in production Windows code. Production Windows code
uses MSVC EH. Today, the only verifiable shape on the SAW MSVC
side is "throw with no catch in the same module" (the
`add_one_throws.cpp` case), which is the *least* interesting EH
shape — anyone who writes a throw without a catch already knows
the function is partial. The interesting shapes — "catch and
recover" (must preserve postcondition) and "catch and remap"
(must rewrite return) — are precisely the ones this pass would
need to handle to be useful, and they are exactly the ones
blocked by Gaps 1–4.

With the four fixes above, every demo in the table at the top
becomes verifiable directly out of the pass. The current
`saw-spec-gen` integration would simply call
`exception-lower in.bc -o out.bc` and pass `out.bc` to
`llvm_load_module` — no `fix_exclow_i8.ps1` workaround needed.

## Appendix — `__cxa_throw` (Itanium) is fine

Sanity check: the Itanium side (`__cxa_throw` / `landingpad` /
`__cxa_begin_catch`) does work. We have not exercised the pass
on `*-pc-linux-gnu` targets ourselves, but the existing SAW
integration test `intTests/test_exception_lower/` suggests the
Itanium rewrites are solid. The gaps above are *specifically*
about the MSVC side, which appears to have been added later and
is missing the rewrites and dispatch table generation that the
Itanium side already implements.
