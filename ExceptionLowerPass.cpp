#include "ExceptionLowerPass.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

namespace exclow {

namespace {

// Names for synthesized instructions and globals.
constexpr StringRef kInFlightFlagName     = "__exclow_error_flag";
constexpr StringRef kThrownTypeInfoName   = "__exclow_error_typeinfo";
constexpr StringRef kThrownValuePtrName   = "__exclow_error_value";
constexpr StringRef kEhCheckBlockSuffix   = ".ehcheck";
constexpr StringRef kErrFlagLabel         = "exclow.err";
constexpr StringRef kTypeInfoLabel        = "exclow.ti";
constexpr StringRef kCaughtPtrLabel       = "exclow.caught";
constexpr StringRef kExnAllocLabel        = "exclow.exn";

// Thread-local globals representing "an exception is currently propagating".
// Together these stand in for the unwinder ABI in lowered bitcode.
struct ErrorState {
  llvm::GlobalVariable *inFlightFlag;       // i1:  true while an exception is live
  llvm::GlobalVariable *thrownTypeInfo;     // ptr: std::type_info of thrown object
  llvm::GlobalVariable *thrownValuePtr;     // ptr: address of the thrown object
};

llvm::GlobalVariable *getOrCreateThreadLocalGlobal(llvm::Module &Mod,
                                                   StringRef GlobalName,
                                                   llvm::Type *GlobalType) {
  if (auto *Existing = Mod.getGlobalVariable(GlobalName))
    return Existing;

  auto *Global = new llvm::GlobalVariable(
      Mod, GlobalType, /*isConstant=*/false, llvm::GlobalValue::InternalLinkage,
      llvm::Constant::getNullValue(GlobalType), GlobalName);
  Global->setThreadLocal(true);
  return Global;
}

ErrorState getOrCreateErrorState(llvm::Module &Mod) {
  auto &Ctx = Mod.getContext();
  auto *BoolTy = llvm::Type::getInt1Ty(Ctx);
  auto *PtrTy = llvm::PointerType::getUnqual(Ctx);
  return {
      getOrCreateThreadLocalGlobal(Mod, kInFlightFlagName,   BoolTy),
      getOrCreateThreadLocalGlobal(Mod, kThrownTypeInfoName, PtrTy),
      getOrCreateThreadLocalGlobal(Mod, kThrownValuePtrName, PtrTy),
  };
}

// A "sentinel" return value used to propagate an error out of an unwinding path.
// For booleans (i1), we use 'false' (null) rather than '-1' to avoid
// accidentally returning 'true' (success) in boolean success-indicator fns.
llvm::Value *makeSentinelReturnValue(llvm::Function &Func) {
  llvm::Type *RetTy = Func.getReturnType();
  if (RetTy->isVoidTy())
    return nullptr;
  if (RetTy->isFloatingPointTy())
    return llvm::ConstantFP::getNaN(RetTy);
  return llvm::Constant::getNullValue(RetTy);
}

// Emit "store true→InFlightFlag; ret <sentinel-or-void>" at Builder.
void emitErrorReturn(llvm::IRBuilder<> &Builder, llvm::Function &Func,
                     const ErrorState &State) {
  Builder.CreateStore(llvm::ConstantInt::getTrue(Builder.getContext()),
                      State.inFlightFlag);
  if (llvm::Value *Sentinel = makeSentinelReturnValue(Func))
    Builder.CreateRet(Sentinel);
  else
    Builder.CreateRetVoid();
}

// Clean up trailing dead code (typically an `unreachable`) after replacing
// a no-return call with a synthesized `ret`.
void eraseTrailingInstructions(llvm::Instruction *StartInst) {
  llvm::BasicBlock *BB = StartInst->getParent();
  while (&BB->back() != StartInst) {
    llvm::Instruction *Dead = &BB->back();
    if (!Dead->use_empty())
      Dead->replaceAllUsesWith(llvm::UndefValue::get(Dead->getType()));
    Dead->eraseFromParent();
  }
}

// Replace `landingpad` with a synthetic `{ ptr, i32 }` built from the
// thread-local typeinfo slot.
void lowerLandingPadInPlace(llvm::LandingPadInst *LP, const ErrorState &State) {
  auto &Ctx = LP->getContext();
  llvm::IRBuilder<> Builder(LP);
  llvm::Value *TI = Builder.CreateLoad(llvm::PointerType::getUnqual(Ctx),
                                       State.thrownTypeInfo, kTypeInfoLabel);
  llvm::Value *Res = llvm::UndefValue::get(LP->getType());
  Res = Builder.CreateInsertValue(Res, TI, 0);
  Res = Builder.CreateInsertValue(
      Res, llvm::ConstantInt::get(llvm::Type::getInt32Ty(Ctx), 0), 1);
  LP->replaceAllUsesWith(Res);
  LP->eraseFromParent();
}

enum class CxaRuntime {
  None,
  AllocateException,
  FreeException,
  Throw,
  Rethrow,
  BeginCatch,
  EndCatch,
};

CxaRuntime classifyCxaCall(const llvm::CallInst &Call) {
  const llvm::Function *Callee = Call.getCalledFunction();
  if (!Callee)
    return CxaRuntime::None;
  return llvm::StringSwitch<CxaRuntime>(Callee->getName())
      .Case("__cxa_allocate_exception", CxaRuntime::AllocateException)
      .Case("__cxa_free_exception",     CxaRuntime::FreeException)
      .Case("__cxa_throw",              CxaRuntime::Throw)
      .Case("__cxa_rethrow",            CxaRuntime::Rethrow)
      .Case("__cxa_begin_catch",        CxaRuntime::BeginCatch)
      .Case("__cxa_end_catch",          CxaRuntime::EndCatch)
      .Default(CxaRuntime::None);
}

// Collector for all instructions that need lowering.
struct LoweringWorklist : llvm::InstVisitor<LoweringWorklist> {
  llvm::SmallVector<llvm::InvokeInst *, 8> Invokes;
  llvm::SmallVector<llvm::ResumeInst *, 4> Resumes;
  llvm::SmallVector<llvm::CallInst *, 8>   CxaCalls;
  llvm::SmallVector<llvm::CatchReturnInst *, 4>   CatchReturns;
  llvm::SmallVector<llvm::CleanupReturnInst *, 4> CleanupReturns;
  llvm::SmallVector<llvm::CatchPadInst *, 4>      CatchPads;
  llvm::SmallVector<llvm::CleanupPadInst *, 4>    CleanupPads;
  llvm::SmallVector<llvm::CatchSwitchInst *, 4>   CatchSwitches;

  void visitInvokeInst(llvm::InvokeInst &I) { Invokes.push_back(&I); }
  void visitResumeInst(llvm::ResumeInst &I) { Resumes.push_back(&I); }
  void visitCallInst(llvm::CallInst &I) {
    if (classifyCxaCall(I) != CxaRuntime::None)
      CxaCalls.push_back(&I);
  }
  void visitCatchReturnInst(llvm::CatchReturnInst &I)     { CatchReturns.push_back(&I); }
  void visitCleanupReturnInst(llvm::CleanupReturnInst &I) { CleanupReturns.push_back(&I); }
  void visitCatchPadInst(llvm::CatchPadInst &I)           { CatchPads.push_back(&I); }
  void visitCleanupPadInst(llvm::CleanupPadInst &I)       { CleanupPads.push_back(&I); }
  void visitCatchSwitchInst(llvm::CatchSwitchInst &I)     { CatchSwitches.push_back(&I); }

  bool empty() const {
    return Invokes.empty() && Resumes.empty() && CxaCalls.empty() &&
           CatchReturns.empty() && CleanupReturns.empty() &&
           CatchPads.empty() && CleanupPads.empty() && CatchSwitches.empty();
  }
};

void lowerInvoke(llvm::InvokeInst *Invoke, const ErrorState &State) {
  auto &Ctx = Invoke->getContext();
  llvm::BasicBlock *NormalDest = Invoke->getNormalDest();
  llvm::BasicBlock *UnwindDest = Invoke->getUnwindDest();
  llvm::BasicBlock *InvokeBB   = Invoke->getParent();
  llvm::Function *Func         = InvokeBB->getParent();

  llvm::BasicBlock *EHBlock = llvm::BasicBlock::Create(
      Ctx, InvokeBB->getName() + kEhCheckBlockSuffix, Func, NormalDest);

  llvm::IRBuilder<> Builder(Invoke);
  llvm::SmallVector<llvm::Value *, 8> Args(Invoke->args());
  llvm::SmallVector<llvm::OperandBundleDef, 2> Bundles;
  Invoke->getOperandBundlesAsDefs(Bundles);

  llvm::CallInst *Call = Builder.CreateCall(Invoke->getFunctionType(),
                                            Invoke->getCalledOperand(), Args,
                                            Bundles);
  Call->setCallingConv(Invoke->getCallingConv());
  Call->setAttributes(Invoke->getAttributes());
  if (!Invoke->getType()->isVoidTy())
    Invoke->replaceAllUsesWith(Call);
  Builder.CreateBr(EHBlock);

  llvm::IRBuilder<> EHBuilder(EHBlock);
  llvm::Value *InFlight = EHBuilder.CreateLoad(
      llvm::Type::getInt1Ty(Ctx), State.inFlightFlag, kErrFlagLabel);
  EHBuilder.CreateCondBr(InFlight, UnwindDest, NormalDest);

  Invoke->eraseFromParent();

  if (auto *LP = llvm::dyn_cast<llvm::LandingPadInst>(&UnwindDest->front()))
    lowerLandingPadInPlace(LP, State);
}

void lowerCxaCall(llvm::CallInst *Call, const ErrorState &State) {
  auto &Ctx = Call->getContext();
  llvm::Function *Func = Call->getFunction();

  switch (classifyCxaCall(*Call)) {
  case CxaRuntime::AllocateException: {
    llvm::IRBuilder<> Builder(Call);
    llvm::Value *Size = Call->getArgOperand(0);
    llvm::Value *Alloca = Builder.CreateAlloca(llvm::Type::getInt8Ty(Ctx), Size,
                                               kExnAllocLabel);
    Call->replaceAllUsesWith(Alloca);
    Call->eraseFromParent();
    break;
  }
  case CxaRuntime::FreeException:
  case CxaRuntime::EndCatch:
    Call->eraseFromParent();
    break;
  case CxaRuntime::Throw: {
    llvm::IRBuilder<> Builder(Call);
    Builder.CreateStore(Call->getArgOperand(1), State.thrownTypeInfo);
    Builder.CreateStore(Call->getArgOperand(0), State.thrownValuePtr);
    emitErrorReturn(Builder, *Func, State);
    eraseTrailingInstructions(Call);
    Call->eraseFromParent();
    break;
  }
  case CxaRuntime::Rethrow: {
    llvm::IRBuilder<> Builder(Call);
    emitErrorReturn(Builder, *Func, State);
    eraseTrailingInstructions(Call);
    Call->eraseFromParent();
    break;
  }
  case CxaRuntime::BeginCatch: {
    llvm::IRBuilder<> Builder(Call);
    llvm::Value *Caught = Builder.CreateLoad(llvm::PointerType::getUnqual(Ctx),
                                             State.thrownValuePtr, kCaughtPtrLabel);
    Builder.CreateStore(llvm::ConstantInt::getFalse(Ctx), State.inFlightFlag);
    Call->replaceAllUsesWith(Caught);
    Call->eraseFromParent();
    break;
  }
  default:
    break;
  }
}

bool lowerFunction(llvm::Function &Func, const ErrorState &State) {
  LoweringWorklist WL;
  WL.visit(Func);
  if (WL.empty())
    return false;

  // 1. Lower Itanium runtime calls.
  for (llvm::CallInst *Call : WL.CxaCalls) {
    if (Call->getParent()) // Might have been erased by a preceding throw.
      lowerCxaCall(Call, State);
  }

  // 2. Lower Invokes (transforms CFG).
  for (llvm::InvokeInst *Invoke : WL.Invokes)
    lowerInvoke(Invoke, State);

  // 3. Lower Resumes.
  for (llvm::ResumeInst *Resume : WL.Resumes) {
    llvm::IRBuilder<> Builder(Resume);
    emitErrorReturn(Builder, Func, State);
    Resume->eraseFromParent();
  }

  // 4. Lower SEH Funclets.
  for (auto *R : WL.CatchReturns) {
    llvm::IRBuilder<> B(R);
    B.CreateStore(llvm::ConstantInt::getFalse(B.getContext()), State.inFlightFlag);
    B.CreateBr(R->getSuccessor());
    R->eraseFromParent();
  }
  for (auto *R : WL.CleanupReturns) {
    llvm::IRBuilder<> B(R);
    if (R->hasUnwindDest())
      B.CreateBr(R->getUnwindDest());
    else
      emitErrorReturn(B, Func, State);
    R->eraseFromParent();
  }
  for (auto *P : WL.CatchPads) {
    llvm::IRBuilder<> B(P);
    B.CreateStore(llvm::ConstantInt::getFalse(B.getContext()), State.inFlightFlag);
    if (!P->use_empty())
      P->replaceAllUsesWith(llvm::UndefValue::get(P->getType()));
    P->eraseFromParent();
  }
  for (auto *P : WL.CleanupPads) {
    if (!P->use_empty())
      P->replaceAllUsesWith(llvm::UndefValue::get(P->getType()));
    P->eraseFromParent();
  }
  for (auto *S : WL.CatchSwitches) {
    llvm::IRBuilder<> B(S);
    if (S->getNumHandlers() > 0)
      B.CreateBr(*S->handler_begin()); // Note: Assumes type-dispatch is handled.
    else if (S->hasUnwindDest())
      B.CreateBr(S->getUnwindDest());
    else
      emitErrorReturn(B, Func, State);
    S->eraseFromParent();
  }

  return true;
}

} // namespace

llvm::PreservedAnalyses ExceptionLowerPass::run(llvm::Module &Mod,
                                                llvm::ModuleAnalysisManager &) {
  const ErrorState State = getOrCreateErrorState(Mod);
  bool Changed = false;

  for (llvm::Function &Func : Mod) {
    if (!Func.isDeclaration())
      Changed |= lowerFunction(Func, State);
  }

  return Changed ? llvm::PreservedAnalyses::none() : llvm::PreservedAnalyses::all();
}

} // namespace exclow

