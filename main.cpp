#include "ExceptionLowerPass.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>

using namespace llvm;

// Returns the LLVM major version that produced `Buf`, if it is LLVM bitcode
// carrying a recognizable producer string, or std::nullopt otherwise (textual
// .ll input, a non-LLVM producer, or bitcode with no identification block).
//
// LLVM writes an identification record of the form "LLVM<major>.<minor>.<patch>"
// (e.g. "LLVM20.1.6") into every bitcode file it emits; we parse the leading
// major version out of it.
static std::optional<unsigned> getBitcodeProducerMajor(MemoryBufferRef Buf) {
  Expected<std::string> Producer = getBitcodeProducerString(Buf);
  if (!Producer) {
    // Not bitcode (e.g. textual IR) or no identification block. There is
    // nothing to guard against, so treat this as "unknown", not an error.
    consumeError(Producer.takeError());
    return std::nullopt;
  }
  StringRef P(*Producer);
  if (!P.consume_front("LLVM"))
    return std::nullopt; // A non-LLVM producer; leave it alone.
  unsigned Major = 0;
  if (P.consumeInteger(10, Major)) // consumeInteger returns true on failure.
    return std::nullopt;
  return Major;
}

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::Required);

static cl::opt<std::string> OutputFilename("o",
                                           cl::desc("Output bitcode file"),
                                           cl::value_desc("filename"),
                                           cl::Required);

static cl::opt<exclow::LoweringMode> Mode(
    "mode", cl::desc("Exception-lowering strategy:"),
    cl::init(exclow::LoweringMode::Auto),
    cl::values(
        clEnumValN(exclow::LoweringMode::Auto, "auto",
                   "Per-function auto-detect (default): pure Win64 SEH "
                   "cleanup funclets (e.g. Rust drop glue) take the "
                   "cleanup-only path, everything else the full lowering."),
        clEnumValN(exclow::LoweringMode::Full, "full",
                   "Force the full Itanium + MSVC error-flag lowering on "
                   "every function."),
        clEnumValN(exclow::LoweringMode::CleanupOnly, "cleanup-only",
                   "Force the cleanup-only lowering on every function: drop "
                   "invoke unwind edges, strip funclet bundles, and delete "
                   "funclet blocks. Catch handlers are conservatively "
                   "dropped.")));

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  ExitOnError ExitOnErr("exception-lower: ");

  cl::ParseCommandLineOptions(
      argc, argv,
      "LLVM C++ exception-lowering pass\n\n"
      "Replaces Itanium and Windows SEH exception-handling constructs\n"
      "with explicit error-flag control flow for SAW verification.\n");

  // Load the input into a buffer so we can inspect its bitcode producer
  // version before handing it to the IR parser.
  ErrorOr<std::unique_ptr<MemoryBuffer>> BufOrErr =
      MemoryBuffer::getFileOrSTDIN(InputFilename);
  if (std::error_code EC = BufOrErr.getError()) {
    errs() << "exception-lower: error reading '" << InputFilename
           << "': " << EC.message() << "\n";
    return 1;
  }
  std::unique_ptr<MemoryBuffer> Buffer = std::move(*BufOrErr);

  // Hard guard against the toolchain-version-mismatch failure mode: LLVM's
  // bitcode format is only *backward* compatible, so an exception-lower linked
  // against LLVM N cannot safely read bitcode produced by LLVM >N. The older
  // reader does not error out -- it best-effort decodes and can silently
  // rewrite valid constructs (e.g. constant GEPs) to `undef`, corrupting IR
  // that has nothing to do with exception handling. Detect that up front and
  // abort with an actionable message instead of emitting broken bitcode.
  if (std::optional<unsigned> ProducerMajor =
          getBitcodeProducerMajor(Buffer->getMemBufferRef())) {
    if (*ProducerMajor > static_cast<unsigned>(LLVM_VERSION_MAJOR)) {
      errs() << "exception-lower: error: input bitcode was produced by LLVM "
             << *ProducerMajor << ", but this exception-lower was built "
                "against LLVM "
             << LLVM_VERSION_MAJOR << ".\n"
             << "  LLVM bitcode is only backward compatible: a newer reader "
                "can read older bitcode,\n"
             << "  but reading newer bitcode with an older LLVM can silently "
                "corrupt IR (e.g.\n"
             << "  constant expressions decoded as 'undef'). Refusing to "
                "continue.\n"
             << "  Rebuild exception-lower against LLVM " << *ProducerMajor
             << " or newer, or recompile the\n"
             << "  input with LLVM " << LLVM_VERSION_MAJOR << " or older.\n";
      return 1;
    }
  }

  // Read input bitcode / IR.
  LLVMContext Ctx;
  SMDiagnostic Diag;
  std::unique_ptr<Module> M = parseIR(Buffer->getMemBufferRef(), Diag, Ctx);
  if (!M) {
    Diag.print(argv[0], errs());
    return 1;
  }

  // Build and run the pass pipeline.
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;

  PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  ModulePassManager MPM;
  MPM.addPass(exclow::ExceptionLowerPass(Mode));
  MPM.run(*M, MAM);

  // Write output bitcode.
  std::error_code EC;
  ToolOutputFile Out(OutputFilename, EC, sys::fs::OF_None);
  if (EC) {
    errs() << "exception-lower: error opening output file: " << EC.message()
           << "\n";
    return 1;
  }

  WriteBitcodeToFile(*M, Out.os());
  Out.keep();

  return 0;
}
