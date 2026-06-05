#ifndef EXCEPTION_LOWER_PASS_H
#define EXCEPTION_LOWER_PASS_H

#include "llvm/IR/PassManager.h"

namespace exclow {

/// Selects how aggressively the pass lowers exception-handling constructs.
enum class LoweringMode {
  /// Per-function auto-detection (the default). A function whose only EH
  /// constructs are Win64 SEH *cleanup* funclets (`cleanuppad` /
  /// `cleanupret`) — with no `catchpad` / `catchswitch` / `landingpad` —
  /// takes the lightweight cleanup-only path. Every other EH-bearing
  /// function takes the full error-flag lowering. This is what Rust's
  /// `rustc`-emitted drop glue needs: the cleanup funclets that block
  /// SAW's bitcode parser are removed, and the resulting straight-line IR
  /// models the panic-free path.
  Auto,
  /// Force the full Itanium + MSVC error-flag lowering on every function,
  /// regardless of shape. This is the historical behaviour.
  Full,
  /// Force the cleanup-only lowering on every EH-bearing function: drop
  /// the unwind edge of every `invoke`, strip `"funclet"` operand bundles,
  /// and delete the now-unreachable funclet blocks. Catch handlers, if
  /// present, are conservatively dropped (the panic-free path never enters
  /// them). Intended for batch pipelines that only need the parse blocker
  /// gone.
  CleanupOnly,
};

/// ExceptionLowerPass - Replaces C++ exception-handling constructs with
/// explicit error-flag control flow so that the resulting bitcode can be
/// analysed by tools (such as SAW) that do not model stack unwinding.
///
/// Handles both the Itanium exception-handling ABI
/// (`invoke` / `landingpad` / `resume` / `__cxa_*`) used on Linux and
/// macOS, and the Windows Structured Exception Handling (SEH) funclet
/// model (`catchswitch` / `catchpad` / `cleanuppad` / `catchret` /
/// `cleanupret`) used by MSVC and `clang-cl`.
///
/// On `x86_64-pc-windows-msvc`, `rustc` emits the same SEH funclet model
/// for `Drop` glue, `panic`, and `catch_unwind`. The cleanup-only mode
/// (see ::LoweringMode) targets that case specifically.
class ExceptionLowerPass
    : public llvm::PassInfoMixin<ExceptionLowerPass> {
public:
  explicit ExceptionLowerPass(LoweringMode Mode = LoweringMode::Auto)
      : Mode(Mode) {}

  llvm::PreservedAnalyses run(llvm::Module &Mod,
                              llvm::ModuleAnalysisManager &);

  static bool isRequired() { return true; }

private:
  LoweringMode Mode;
};

} // namespace exclow

#endif // EXCEPTION_LOWER_PASS_H
