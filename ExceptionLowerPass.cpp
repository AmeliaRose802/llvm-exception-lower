#include "ExceptionLowerPass.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
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

// Names for synthesized slots and blocks.
constexpr StringRef kInFlightFlagName       = "__exclow_error_flag";
constexpr StringRef kThrownTypeInfoName     = "__exclow_error_typeinfo";
constexpr StringRef kThrownValuePtrName     = "__exclow_error_value";
constexpr StringRef kEhCheckBlockSuffix     = ".ehcheck";
constexpr StringRef kErrFlagLabel           = "exclow.err";
constexpr StringRef kTypeInfoLabel          = "exclow.ti";
constexpr StringRef kCaughtPtrLabel         = "exclow.caught";
constexpr StringRef kExnAllocLabel          = "exclow.exn";
constexpr StringRef kCatchMatchLabel        = "exclow.match";
constexpr StringRef kCatchNextBlockName     = "exclow.catch.next";
constexpr StringRef kUnhandledBlockName     = "exclow.unhandled";
constexpr StringRef kCanonicalTypeDescPrefix = "__exclow.td.";

// Module-level globals representing "an exception is currently propagating".
// Using module globals (rather than per-function allocas) lets exception
// state propagate across call boundaries: when a callee throws and
// returns a sentinel, the caller's .ehcheck block can read the same flag.
//
// Downstream tools that require explicit global allocation (e.g. SAW's
// crucible-llvm) must emit `llvm_alloc_global` for these names; the
// saw-spec-gen tool does this automatically via `inject_exclow_globals`.
struct ErrorState {
  GlobalVariable *inFlightFlag;    // i1:  true while an exception is live
  GlobalVariable *thrownTypeInfo;  // ptr: std::type_info of thrown object
  GlobalVariable *thrownValuePtr;  // ptr: address of the thrown object
};

GlobalVariable *getOrCreateGlobal(Module &Mod, StringRef Name, Type *Ty) {
  if (auto *Existing = Mod.getGlobalVariable(Name))
    return Existing;
  return new GlobalVariable(Mod, Ty, /*isConstant=*/false,
                            GlobalValue::InternalLinkage,
                            Constant::getNullValue(Ty), Name);
}

ErrorState getOrCreateErrorState(Module &Mod) {
  auto &Ctx = Mod.getContext();
  return {
      getOrCreateGlobal(Mod, kInFlightFlagName,   Type::getInt1Ty(Ctx)),
      getOrCreateGlobal(Mod, kThrownTypeInfoName, PointerType::getUnqual(Ctx)),
      getOrCreateGlobal(Mod, kThrownValuePtrName, PointerType::getUnqual(Ctx)),
  };
}

// A "sentinel" return value used when an exception escapes the function.
// For booleans (i1) we use `false` (null) rather than `-1` so that we do not
// accidentally return `true` (success) for boolean success-indicator
// functions.
Value *makeSentinelReturnValue(Function &Func) {
  Type *RetTy = Func.getReturnType();
  if (RetTy->isVoidTy())
    return nullptr;
  if (RetTy->isFloatingPointTy())
    return ConstantFP::getNaN(RetTy);
  return Constant::getNullValue(RetTy);
}

// Emit "store true→InFlightFlag; ret <sentinel-or-void>" at Builder.
void emitErrorReturn(IRBuilder<> &Builder, Function &Func,
                     const ErrorState &State) {
  Builder.CreateStore(ConstantInt::getTrue(Builder.getContext()),
                      State.inFlightFlag);
  if (Value *Sentinel = makeSentinelReturnValue(Func))
    Builder.CreateRet(Sentinel);
  else
    Builder.CreateRetVoid();
}

// Clean up trailing dead code (typically an `unreachable`) after replacing
// a no-return call with a synthesized `ret`.
void eraseTrailingInstructions(Instruction *StartInst) {
  BasicBlock *BB = StartInst->getParent();
  while (&BB->back() != StartInst) {
    Instruction *Dead = &BB->back();
    if (!Dead->use_empty())
      Dead->replaceAllUsesWith(UndefValue::get(Dead->getType()));
    Dead->eraseFromParent();
  }
}

// Replace a `landingpad` with a synthetic `{ ptr, i32 }` built from the
// in-flight typeinfo slot.
void lowerLandingPadInPlace(LandingPadInst *LP, const ErrorState &State) {
  LLVMContext &Ctx = LP->getContext();
  IRBuilder<> Builder(LP);
  Value *TI = Builder.CreateLoad(PointerType::getUnqual(Ctx),
                                 State.thrownTypeInfo, kTypeInfoLabel);
  Value *Res = UndefValue::get(LP->getType());
  Res = Builder.CreateInsertValue(Res, TI, 0);
  Res = Builder.CreateInsertValue(
      Res, ConstantInt::get(Type::getInt32Ty(Ctx), 0), 1);
  LP->replaceAllUsesWith(Res);
  LP->eraseFromParent();
}

// ---------------------------------------------------------------------------
// MSVC type-discriminator canonicalisation.
//
// Clang's MSVC-target lowering emits two *different* RTTI globals for the
// "what type was thrown" question:
//
//   * Throw site: `_CxxThrowException(value, @"_TI<flags><mangled-tag>")` —
//     a `%eh.ThrowInfo` struct that points (via `_CTA*` and `_CT*`) at the
//     real type descriptor.
//   * Catch site: `catchpad within %cs [ptr @"??_R0<mangled-tag>@8", ...]` —
//     the actual `%rtti.TypeDescriptor`.
//
// Pointer-equality on the raw operands therefore *never* matches. To make
// the typed dispatch work without running an MSVC RTTI interpreter at
// verification time, we route both sides through a single canonical
// per-type-tag symbol: `@__exclow.td.<mangled-tag>`. The canonical symbol
// has `internal` linkage and a trivial initialiser — only its address is
// meaningful, never its contents.
//
// As a bonus this also fixes the second SAW-side issue: the original
// `??_R0` globals are emitted as `linkonce_odr` + `comdat` and crucible-
// llvm declines to allocate them. The internal clone we substitute is the
// `internal constant` linkage pattern that crucible-llvm accepts.

// Strip `_TI<digits>` prefix from a throw-info global's name, returning
// the trailing mangled type tag (e.g. `?AUHarmlessTag@@`, `H` for `int`).
StringRef extractTagFromThrowInfo(StringRef Name) {
  if (!Name.consume_front("_TI"))
    return StringRef();
  while (!Name.empty() && Name.front() >= '0' && Name.front() <= '9')
    Name = Name.drop_front();
  return Name;
}

// Strip `??_R0` prefix and `@8` suffix from a type-descriptor global's
// name, returning the mangled type tag.
StringRef extractTagFromTypeDescriptor(StringRef Name) {
  if (!Name.consume_front("??_R0"))
    return StringRef();
  if (!Name.consume_back("@8"))
    return StringRef();
  return Name;
}

// Return the canonical per-type-tag global, creating it on first use.
GlobalVariable *getOrCreateCanonicalTypeDescriptor(Module &Mod,
                                                   StringRef Tag) {
  SmallString<128> NameBuf;
  NameBuf.append(kCanonicalTypeDescPrefix);
  NameBuf.append(Tag);

  if (auto *Existing = Mod.getNamedGlobal(NameBuf))
    return Existing;

  LLVMContext &Ctx = Mod.getContext();
  Type *I8 = Type::getInt8Ty(Ctx);
  auto *G = new GlobalVariable(Mod, I8,
                               /*isConstant=*/true,
                               GlobalValue::InternalLinkage,
                               ConstantInt::get(I8, 0), NameBuf);
  G->setAlignment(MaybeAlign(1));
  G->setDSOLocal(true);
  return G;
}

// Given a value that should denote "the thrown type" (the throw-info or
// type-descriptor operand of a throw / catch site), return the canonical
// per-tag global to use in its place. Returns the input unchanged if the
// operand doesn't match the MSVC name patterns we recognise.
Value *canonicaliseThrowInfo(Module &Mod, Value *Op) {
  auto *Glob = dyn_cast<GlobalVariable>(Op->stripPointerCasts());
  if (!Glob)
    return Op;
  StringRef Tag = extractTagFromThrowInfo(Glob->getName());
  if (Tag.empty())
    return Op;
  return getOrCreateCanonicalTypeDescriptor(Mod, Tag);
}

Value *canonicaliseTypeDescriptor(Module &Mod, Value *Op) {
  auto *Glob = dyn_cast<GlobalVariable>(Op->stripPointerCasts());
  if (!Glob)
    return Op;
  StringRef Tag = extractTagFromTypeDescriptor(Glob->getName());
  if (Tag.empty())
    return Op;
  return getOrCreateCanonicalTypeDescriptor(Mod, Tag);
}

enum class CxaRuntime {
  None,
  AllocateException,
  FreeException,
  Throw,         // Itanium __cxa_throw
  Rethrow,       // Itanium __cxa_rethrow
  BeginCatch,
  EndCatch,
  CxxThrow,      // MSVC _CxxThrowException
};

CxaRuntime classifyCxa(const Function *Callee) {
  if (!Callee)
    return CxaRuntime::None;
  return StringSwitch<CxaRuntime>(Callee->getName())
      .Case("__cxa_allocate_exception", CxaRuntime::AllocateException)
      .Case("__cxa_free_exception",     CxaRuntime::FreeException)
      .Case("__cxa_throw",              CxaRuntime::Throw)
      .Case("__cxa_rethrow",            CxaRuntime::Rethrow)
      .Case("__cxa_begin_catch",        CxaRuntime::BeginCatch)
      .Case("__cxa_end_catch",          CxaRuntime::EndCatch)
      .Case("_CxxThrowException",       CxaRuntime::CxxThrow)
      .Default(CxaRuntime::None);
}

bool isThrowKind(CxaRuntime K) {
  return K == CxaRuntime::Throw || K == CxaRuntime::Rethrow ||
         K == CxaRuntime::CxxThrow;
}

// Lower a throw-like call site.
//
// All throw-like calls publish "exception in flight" to the error-state
// slots. If the throw is followed by `unreachable` (so the throw escapes
// the function), we additionally emit a sentinel `ret`. If the throw is
// followed by a normal terminator — typically a `br %.ehcheck` left behind
// when an earlier `invoke` of the throw runtime was demoted — we leave that
// terminator alone so the caller's flag-check picks up the in-flight state.
void lowerThrowCall(CallInst *Call, const ErrorState &State) {
  Function *Func = Call->getFunction();
  LLVMContext &Ctx = Call->getContext();
  IRBuilder<> B(Call);

  CxaRuntime K = classifyCxa(Call->getCalledFunction());
  if (K == CxaRuntime::Throw) {
    // __cxa_throw(value, typeinfo, dtor). The Itanium typeinfo pointer is
    // already the same global the matching landingpad clause / typeid.for
    // intrinsic see, so no remapping is required.
    B.CreateStore(Call->getArgOperand(1), State.thrownTypeInfo);
    B.CreateStore(Call->getArgOperand(0), State.thrownValuePtr);
  } else if (K == CxaRuntime::CxxThrow) {
    // _CxxThrowException(value, throw-info). The throw-info global is a
    // different RTTI symbol from the catchpad's type descriptor; route
    // both sides through a canonical per-type-tag symbol so the typed
    // dispatch's `icmp eq` can actually match.
    Value *TIArg = canonicaliseThrowInfo(*Func->getParent(),
                                         Call->getArgOperand(1));
    B.CreateStore(TIArg, State.thrownTypeInfo);
    B.CreateStore(Call->getArgOperand(0), State.thrownValuePtr);
  }
  // For Rethrow the slots already hold the in-flight values.
  B.CreateStore(ConstantInt::getTrue(Ctx), State.inFlightFlag);

  Instruction *Next = Call->getNextNode();
  if (Next && isa<UnreachableInst>(Next)) {
    emitErrorReturn(B, *Func, State);
    eraseTrailingInstructions(Call);
  }
  Call->eraseFromParent();
}

// Lower the non-throw flavors of the Itanium runtime.
void lowerCxaCall(CallInst *Call, const ErrorState &State) {
  LLVMContext &Ctx = Call->getContext();

  switch (classifyCxa(Call->getCalledFunction())) {
  case CxaRuntime::AllocateException: {
    IRBuilder<> Builder(Call);
    Value *Size = Call->getArgOperand(0);
    Value *Alloca = Builder.CreateAlloca(Type::getInt8Ty(Ctx), Size,
                                         kExnAllocLabel);
    Call->replaceAllUsesWith(Alloca);
    Call->eraseFromParent();
    break;
  }
  case CxaRuntime::FreeException:
  case CxaRuntime::EndCatch:
    Call->eraseFromParent();
    break;
  case CxaRuntime::BeginCatch: {
    IRBuilder<> Builder(Call);
    Value *Caught = Builder.CreateLoad(PointerType::getUnqual(Ctx),
                                       State.thrownValuePtr, kCaughtPtrLabel);
    Builder.CreateStore(ConstantInt::getFalse(Ctx), State.inFlightFlag);
    Call->replaceAllUsesWith(Caught);
    Call->eraseFromParent();
    break;
  }
  default:
    break;
  }
}

void lowerInvoke(InvokeInst *Invoke, const ErrorState &State) {
  LLVMContext &Ctx = Invoke->getContext();
  BasicBlock *NormalDest = Invoke->getNormalDest();
  BasicBlock *UnwindDest = Invoke->getUnwindDest();
  BasicBlock *InvokeBB   = Invoke->getParent();
  Function *Func         = InvokeBB->getParent();

  BasicBlock *EHBlock = BasicBlock::Create(
      Ctx, InvokeBB->getName() + kEhCheckBlockSuffix, Func, NormalDest);

  IRBuilder<> Builder(Invoke);
  SmallVector<Value *, 8> Args(Invoke->args());
  SmallVector<OperandBundleDef, 2> Bundles;
  Invoke->getOperandBundlesAsDefs(Bundles);

  CallInst *Call = Builder.CreateCall(Invoke->getFunctionType(),
                                      Invoke->getCalledOperand(), Args,
                                      Bundles);
  Call->setCallingConv(Invoke->getCallingConv());
  Call->setAttributes(Invoke->getAttributes());
  if (!Invoke->getType()->isVoidTy())
    Invoke->replaceAllUsesWith(Call);
  Builder.CreateBr(EHBlock);

  IRBuilder<> EHBuilder(EHBlock);
  Value *InFlight = EHBuilder.CreateLoad(Type::getInt1Ty(Ctx),
                                         State.inFlightFlag, kErrFlagLabel);
  EHBuilder.CreateCondBr(InFlight, UnwindDest, NormalDest);

  Invoke->eraseFromParent();

  if (auto *LP = dyn_cast<LandingPadInst>(&UnwindDest->front()))
    lowerLandingPadInPlace(LP, State);
}

// Strip `"funclet"` operand bundles from every call / invoke in the function.
//
// After funclet lowering, the parent catchpad / cleanuppad token has been
// replaced with `undef`, but every call that lived inside the funclet still
// carries an `[ "funclet"(token undef) ]` annotation on the call itself.
// SAW's bitcode parser does not yet decode FUNC_CODE_OPERAND_BUNDLE and
// rejects the module on first encounter. Drop the bundles to keep the
// output portable.
void stripFuncletBundles(Function &Func) {
  SmallVector<CallBase *, 16> ToRewrite;
  for (BasicBlock &BB : Func) {
    for (Instruction &I : BB) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      for (unsigned i = 0, e = CB->getNumOperandBundles(); i < e; ++i) {
        if (CB->getOperandBundleAt(i).getTagName() == "funclet") {
          ToRewrite.push_back(CB);
          break;
        }
      }
    }
  }
  for (CallBase *CB : ToRewrite) {
    SmallVector<OperandBundleDef, 2> Keep;
    for (unsigned i = 0, e = CB->getNumOperandBundles(); i < e; ++i) {
      OperandBundleUse OB = CB->getOperandBundleAt(i);
      if (OB.getTagName() != "funclet")
        Keep.emplace_back(OB);
    }
    CallBase *NewCB = CallBase::Create(CB, Keep, CB);
    NewCB->takeName(CB);
    if (!CB->use_empty())
      CB->replaceAllUsesWith(NewCB);
    CB->eraseFromParent();
  }
}

struct LoweringWorklist : InstVisitor<LoweringWorklist> {
  SmallVector<InvokeInst *, 8>        Invokes;
  SmallVector<ResumeInst *, 4>        Resumes;
  SmallVector<CallInst *, 8>          CxaCalls; // non-throw flavors
  SmallVector<CatchReturnInst *, 4>   CatchReturns;
  SmallVector<CleanupReturnInst *, 4> CleanupReturns;
  SmallVector<CatchPadInst *, 4>      CatchPads;
  SmallVector<CleanupPadInst *, 4>    CleanupPads;
  SmallVector<CatchSwitchInst *, 4>   CatchSwitches;

  void visitInvokeInst(InvokeInst &I)             { Invokes.push_back(&I); }
  void visitResumeInst(ResumeInst &I)             { Resumes.push_back(&I); }
  void visitCallInst(CallInst &I) {
    CxaRuntime K = classifyCxa(I.getCalledFunction());
    // Throw-like calls are picked up by a separate post-invoke-lowering scan
    // so that invokes-of-throw-runtimes (common in MSVC `try` blocks) get
    // their resulting `call` processed too.
    if (K != CxaRuntime::None && !isThrowKind(K))
      CxaCalls.push_back(&I);
  }
  void visitCatchReturnInst(CatchReturnInst &I)     { CatchReturns.push_back(&I); }
  void visitCleanupReturnInst(CleanupReturnInst &I) { CleanupReturns.push_back(&I); }
  void visitCatchPadInst(CatchPadInst &I)           { CatchPads.push_back(&I); }
  void visitCleanupPadInst(CleanupPadInst &I)       { CleanupPads.push_back(&I); }
  void visitCatchSwitchInst(CatchSwitchInst &I)     { CatchSwitches.push_back(&I); }

  bool empty() const {
    return Invokes.empty() && Resumes.empty() && CxaCalls.empty() &&
           CatchReturns.empty() && CleanupReturns.empty() &&
           CatchPads.empty() && CleanupPads.empty() && CatchSwitches.empty();
  }
};

// Quick check: does the function contain anything for the pass to lower?
bool functionHasEHWork(Function &Func) {
  for (BasicBlock &BB : Func) {
    for (Instruction &I : BB) {
      if (isa<InvokeInst>(&I)        || isa<ResumeInst>(&I)        ||
          isa<CatchReturnInst>(&I)   || isa<CleanupReturnInst>(&I) ||
          isa<CatchPadInst>(&I)      || isa<CleanupPadInst>(&I)    ||
          isa<CatchSwitchInst>(&I)   || isa<LandingPadInst>(&I))
        return true;
      if (auto *C = dyn_cast<CallInst>(&I))
        if (classifyCxa(C->getCalledFunction()) != CxaRuntime::None)
          return true;
    }
  }
  return false;
}

// Collect throw-like calls. Re-scanned after invoke lowering so that
// invokes-of-throw-runtimes get their demoted `call` picked up.
SmallVector<CallInst *, 8> collectThrowCalls(Function &Func) {
  SmallVector<CallInst *, 8> Out;
  for (BasicBlock &BB : Func) {
    for (Instruction &I : BB) {
      auto *C = dyn_cast<CallInst>(&I);
      if (!C)
        continue;
      if (isThrowKind(classifyCxa(C->getCalledFunction())))
        Out.push_back(C);
    }
  }
  return Out;
}

// Snapshot of a catchswitch and its per-handler type filters. Captured
// while catchpads are still alive so the catchpad's first argument (the
// type descriptor) can be read.
struct CatchSwitchSnapshot {
  CatchSwitchInst *Switch;
  // For each handler: (handler-block, type-descriptor or nullptr for catch-all).
  SmallVector<std::pair<BasicBlock *, Value *>, 4> Handlers;
};

SmallVector<CatchSwitchSnapshot, 4>
snapshotCatchSwitches(ArrayRef<CatchSwitchInst *> Switches) {
  SmallVector<CatchSwitchSnapshot, 4> Out;
  for (CatchSwitchInst *S : Switches) {
    Module &Mod = *S->getFunction()->getParent();
    CatchSwitchSnapshot Snap;
    Snap.Switch = S;
    for (auto It = S->handler_begin(); It != S->handler_end(); ++It) {
      BasicBlock *H = *It;
      Value *TD = nullptr;
      if (auto *CP = dyn_cast<CatchPadInst>(&H->front())) {
        if (CP->arg_size() > 0) {
          Value *Op0 = CP->getArgOperand(0);
          // MSVC encodes `catch (...)` with a null type descriptor.
          if (!isa<ConstantPointerNull>(Op0))
            TD = canonicaliseTypeDescriptor(Mod, Op0);
        }
      }
      Snap.Handlers.push_back({H, TD});
    }
    Out.push_back(std::move(Snap));
  }
  return Out;
}

// Lower a catchswitch to a typed dispatch chain:
//
//   %ti = load ptr, ptr %typeinfo_slot
//   %m0 = icmp eq ptr %ti, @TypeDescriptor0
//   br i1 %m0, label %handler0, label %try1
//   try1:
//   %m1 = icmp eq ptr %ti, @TypeDescriptor1
//   br i1 %m1, label %handler1, label %unhandled
//
// A handler with a null type descriptor (i.e. `catch (...)`) becomes an
// unconditional branch and shadows any later handlers (matching C++'s
// in-order dispatch).
//
// "Unhandled" branches to the catchswitch's unwind destination if it has
// one, otherwise to a freshly-synthesized block that calls emitErrorReturn.
void lowerCatchSwitch(const CatchSwitchSnapshot &Snap, Function &Func,
                      const ErrorState &State) {
  CatchSwitchInst *S = Snap.Switch;
  LLVMContext &Ctx = S->getContext();
  IRBuilder<> B(S);
  Type *PtrTy = PointerType::getUnqual(Ctx);

  BasicBlock *Unhandled = nullptr;
  if (S->hasUnwindDest())
    Unhandled = S->getUnwindDest();

  auto ensureUnhandled = [&]() {
    if (Unhandled)
      return;
    Unhandled = BasicBlock::Create(Ctx, kUnhandledBlockName, &Func);
    IRBuilder<> UB(Unhandled);
    emitErrorReturn(UB, Func, State);
  };

  if (Snap.Handlers.empty()) {
    ensureUnhandled();
    B.CreateBr(Unhandled);
    S->eraseFromParent();
    return;
  }

  Value *TI = B.CreateLoad(PtrTy, State.thrownTypeInfo, kTypeInfoLabel);

  for (size_t i = 0, n = Snap.Handlers.size(); i < n; ++i) {
    BasicBlock *H = Snap.Handlers[i].first;
    Value *TD    = Snap.Handlers[i].second;

    // catch-all: unconditional branch, subsequent arms unreachable.
    if (!TD) {
      B.CreateBr(H);
      S->eraseFromParent();
      return;
    }

    Value *Match = B.CreateICmpEQ(TI, TD, kCatchMatchLabel);
    bool isLast = (i + 1 == n);
    if (isLast) {
      ensureUnhandled();
      B.CreateCondBr(Match, H, Unhandled);
    } else {
      BasicBlock *Next =
          BasicBlock::Create(Ctx, kCatchNextBlockName, &Func);
      B.CreateCondBr(Match, H, Next);
      B.SetInsertPoint(Next);
    }
  }

  S->eraseFromParent();
}

// Replace `call i32 @llvm.eh.typeid.for(ptr @TypeInfo)` with a direct
// pointer comparison against the stored typeinfo.  The landing-pad
// lowering sets the selector to 0 for every thrown type, so the original
// `icmp eq i32 %sel, %tid` can never match.  Instead we find the icmp
// that uses the typeid result and rewrite it to compare the thrown
// typeinfo pointer directly: `icmp eq ptr <thrown_ti>, @TypeInfo`.
void lowerEhTypeidFor(Function &Func, const ErrorState &State) {
  SmallVector<CallInst *, 4> TypeIdCalls;
  for (auto &BB : Func)
    for (auto &I : BB)
      if (auto *CI = dyn_cast<CallInst>(&I))
        if (CI->getCalledFunction() &&
            CI->getCalledFunction()->getName().starts_with(
                "llvm.eh.typeid.for"))
          TypeIdCalls.push_back(CI);

  for (CallInst *TID : TypeIdCalls) {
    Value *CatchTypeInfo = TID->getArgOperand(0);
    SmallVector<ICmpInst *, 2> IcmpUsers;
    for (User *U : TID->users())
      if (auto *IC = dyn_cast<ICmpInst>(U))
        IcmpUsers.push_back(IC);
    for (ICmpInst *IC : IcmpUsers) {
      IRBuilder<> B(IC);
      Value *ThrownTI = B.CreateLoad(
          PointerType::getUnqual(Func.getContext()),
          State.thrownTypeInfo, "exclow.thrown.ti");
      Value *Match = B.CreateICmpEQ(ThrownTI, CatchTypeInfo,
                                    kCatchMatchLabel);
      IC->replaceAllUsesWith(Match);
      IC->eraseFromParent();
    }
    if (TID->use_empty())
      TID->eraseFromParent();
  }
}

// ---------------------------------------------------------------------------
// Cleanup-only lowering (Rust drop-glue path).
//
// On `x86_64-pc-windows-msvc`, rustc emits Win64 SEH *cleanup* funclets for
// `Drop` glue: an `invoke` whose unwind edge targets a `cleanuppad` block,
// with `cleanupret ... unwind to caller` and `[ "funclet"(token %cp) ]`
// bundles on the calls inside. There is no `catchpad` / `catchswitch` — Rust
// drop glue never *catches*. SAW's `llvm-pretty-bc-parser` rejects the
// `"funclet"` operand bundle before any simulation can run.
//
// The full error-flag lowering would work, but it threads a global in-flight
// flag through every call site, producing branchy IR. For the panic-free
// verified path the cleanup funclets never execute, so we can do something
// much simpler and cleaner: drop each invoke's unwind edge (turning it into a
// plain `call` + `br` to the normal destination), strip the funclet bundles,
// clear the personality, and delete the now-unreachable funclet blocks. The
// result is straight-line code with no error-state globals at all.

// Does this function's EH work consist solely of cleanup funclets (no catch
// dispatch of any kind)? Such functions take the lightweight cleanup-only
// path under LoweringMode::Auto.
bool isCleanupOnlyFunction(Function &Func) {
  bool HasCleanup = false;
  for (BasicBlock &BB : Func) {
    for (Instruction &I : BB) {
      if (isa<CleanupPadInst>(&I) || isa<CleanupReturnInst>(&I)) {
        HasCleanup = true;
        continue;
      }
      // Any catch-style construct (MSVC or Itanium) disqualifies the
      // function from the cleanup-only path — those need the typed dispatch
      // the full lowering provides.
      if (isa<CatchPadInst>(&I) || isa<CatchSwitchInst>(&I) ||
          isa<CatchReturnInst>(&I) || isa<LandingPadInst>(&I) ||
          isa<ResumeInst>(&I))
        return false;
    }
  }
  return HasCleanup;
}

// Convert an `invoke` into a plain `call` followed by an unconditional branch
// to its normal destination, discarding the unwind edge entirely. Returns the
// synthesized call. The unwind destination loses this predecessor (its PHIs,
// if any, are fixed up).
CallInst *demoteInvokeDroppingUnwind(InvokeInst *Invoke) {
  BasicBlock *NormalDest = Invoke->getNormalDest();
  BasicBlock *UnwindDest = Invoke->getUnwindDest();
  BasicBlock *InvokeBB = Invoke->getParent();

  IRBuilder<> Builder(Invoke);
  SmallVector<Value *, 8> Args(Invoke->args());
  SmallVector<OperandBundleDef, 2> Bundles;
  Invoke->getOperandBundlesAsDefs(Bundles);

  CallInst *Call = Builder.CreateCall(Invoke->getFunctionType(),
                                      Invoke->getCalledOperand(), Args,
                                      Bundles);
  Call->setCallingConv(Invoke->getCallingConv());
  Call->setAttributes(Invoke->getAttributes());
  if (!Invoke->getType()->isVoidTy())
    Invoke->replaceAllUsesWith(Call);
  Builder.CreateBr(NormalDest);

  // Drop the unwind edge: remove this block as a predecessor of the funclet
  // landing block so its PHIs (if any) stay well-formed before it is deleted.
  UnwindDest->removePredecessor(InvokeBB);

  Invoke->eraseFromParent();
  return Call;
}

// Lower a function using the cleanup-only strategy. Returns true if anything
// changed. Does not touch or require the module-level error-state globals.
bool lowerFunctionCleanupOnly(Function &Func) {
  if (!functionHasEHWork(Func))
    return false;

  // 1. Demote every invoke to a call + branch to the normal destination,
  //    discarding the unwind edge. This severs the only way control could
  //    reach the funclet blocks.
  SmallVector<InvokeInst *, 8> Invokes;
  for (BasicBlock &BB : Func)
    for (Instruction &I : BB)
      if (auto *II = dyn_cast<InvokeInst>(&I))
        Invokes.push_back(II);
  for (InvokeInst *II : Invokes)
    demoteInvokeDroppingUnwind(II);

  // 2. Strip `"funclet"` operand bundles from every remaining call. The pad
  //    tokens they reference are about to disappear, and SAW's bitcode
  //    parser rejects the bundle regardless.
  stripFuncletBundles(Func);

  // 3. The personality function (MSVC `__CxxFrameHandler3`) is now dead.
  if (Func.hasPersonalityFn())
    Func.setPersonalityFn(nullptr);

  // 4. Delete the funclet blocks, which are now unreachable from entry.
  //    EliminateUnreachableBlocks replaces cross-block token uses (e.g. a
  //    `cleanupret from %cleanuppad`) with `undef` as it removes the blocks,
  //    so the leftover `cleanuppad` / `cleanupret` instructions go with them.
  EliminateUnreachableBlocks(Func);

  return true;
}

bool lowerFunction(Function &Func, const ErrorState &State) {
  if (!functionHasEHWork(Func))
    return false;

  LoweringWorklist WL;
  WL.visit(Func);

  // 1. Capture catchswitch handler info while catchpads are still alive.
  auto Snapshots = snapshotCatchSwitches(WL.CatchSwitches);

  // 2. Lower non-throw Itanium runtime calls.
  for (CallInst *Call : WL.CxaCalls)
    if (Call->getParent())
      lowerCxaCall(Call, State);

  // 3. Lower Invokes (transforms CFG, may demote throw-invokes into calls).
  for (InvokeInst *Invoke : WL.Invokes)
    lowerInvoke(Invoke, State);

  // 3b. Re-scan for CXA runtime calls that were originally `invoke`s and
  //     got demoted to `call`s by step 3.  The worklist built before
  //     invoke lowering only captures pre-existing CallInsts, so any
  //     __cxa_begin_catch / __cxa_end_catch / __cxa_free_exception that
  //     were invoked (common in nested try/catch) would be missed.
  for (auto &BB : Func)
    for (auto I = BB.begin(); I != BB.end(); ) {
      Instruction &Inst = *I++;
      auto *CI = dyn_cast<CallInst>(&Inst);
      if (!CI || !CI->getCalledFunction()) continue;
      CxaRuntime K = classifyCxa(CI->getCalledFunction());
      if (K != CxaRuntime::None && !isThrowKind(K))
        lowerCxaCall(CI, State);
    }

  // 4. Lower throw-like calls — both __cxa_throw / __cxa_rethrow and the
  //    MSVC counterpart _CxxThrowException — AFTER invoke lowering, so
  //    previously-Invoke throw sites get their demoted `call` rewritten.
  for (CallInst *Call : collectThrowCalls(Func))
    lowerThrowCall(Call, State);

  // 5. Lower Resumes.
  for (ResumeInst *Resume : WL.Resumes) {
    IRBuilder<> Builder(Resume);
    emitErrorReturn(Builder, Func, State);
    Resume->eraseFromParent();
  }

  // 6. Lower SEH funclet edges (catchret / cleanupret). Done before
  //    catchpad/cleanuppad erasure so the funclet-edge instructions can
  //    still reach their parent pad.
  for (auto *R : WL.CatchReturns) {
    IRBuilder<> B(R);
    B.CreateStore(ConstantInt::getFalse(B.getContext()), State.inFlightFlag);
    B.CreateBr(R->getSuccessor());
    R->eraseFromParent();
  }
  for (auto *R : WL.CleanupReturns) {
    IRBuilder<> B(R);
    if (R->hasUnwindDest())
      B.CreateBr(R->getUnwindDest());
    else
      emitErrorReturn(B, Func, State);
    R->eraseFromParent();
  }

  // 7. Lower catchpads / cleanuppads. Must happen before catchswitch
  //    erasure so the catchswitch's token-typed uses are dropped.
  for (auto *P : WL.CatchPads) {
    IRBuilder<> B(P);
    B.CreateStore(ConstantInt::getFalse(B.getContext()), State.inFlightFlag);
    if (!P->use_empty())
      P->replaceAllUsesWith(UndefValue::get(P->getType()));
    P->eraseFromParent();
  }
  for (auto *P : WL.CleanupPads) {
    if (!P->use_empty())
      P->replaceAllUsesWith(UndefValue::get(P->getType()));
    P->eraseFromParent();
  }

  // 8. Lower catchswitches with typed dispatch using the captured snapshot.
  for (const auto &Snap : Snapshots)
    lowerCatchSwitch(Snap, Func, State);

  // 9. Drop the function's personality attribute. Catchpads / landingpads
  //    are gone, so the personality function reference is dead — and on
  //    MSVC it's an external `declare` for `__CxxFrameHandler3` that
  //    downstream tools would otherwise have to model.
  if (Func.hasPersonalityFn())
    Func.setPersonalityFn(nullptr);

  // 10. Strip leftover `"funclet"(...)` operand bundles on calls that used
  //     to live inside a funclet. SAW's bitcode parser does not yet decode
  //     FUNC_CODE_OPERAND_BUNDLE; without this, the lowered module fails
  //     to load.
  stripFuncletBundles(Func);

  // 11. Eliminate llvm.eh.typeid.for intrinsics left over from Itanium
  //     landing-pad lowering. Must run after lowerInvoke so that the
  //     global error-state variables are already wired up.
  lowerEhTypeidFor(Func, State);

  return true;
}

} // namespace

PreservedAnalyses ExceptionLowerPass::run(Module &Mod,
                                          ModuleAnalysisManager &) {
  // Decide, per function, which lowering path it will take. A function takes
  // the cleanup-only path when the mode forces it, or — under Auto — when its
  // EH work is purely Win64 SEH cleanup funclets (Rust drop glue). Everything
  // else takes the full Itanium + MSVC error-flag lowering.
  auto usesCleanupOnly = [&](Function &Func) {
    switch (Mode) {
    case LoweringMode::Full:
      return false;
    case LoweringMode::CleanupOnly:
      return true;
    case LoweringMode::Auto:
      return isCleanupOnlyFunction(Func);
    }
    return false;
  };

  // The module-level error-state globals are only needed by the full path;
  // the cleanup-only path emits straight-line code with no error state. Scan
  // first so a module of pure drop-glue functions does not get the globals
  // (which would otherwise force downstream tools to model them).
  bool NeedsFullState = false;
  bool HasAnyEHWork = false;
  for (Function &Func : Mod) {
    if (Func.isDeclaration() || !functionHasEHWork(Func))
      continue;
    HasAnyEHWork = true;
    if (!usesCleanupOnly(Func)) {
      NeedsFullState = true;
      break;
    }
  }
  if (!HasAnyEHWork)
    return PreservedAnalyses::all();

  // Only materialise the error-state globals when the full path will run.
  bool HaveState = NeedsFullState;
  ErrorState State{};
  if (HaveState)
    State = getOrCreateErrorState(Mod);

  bool Changed = false;
  for (Function &Func : Mod) {
    if (Func.isDeclaration())
      continue;
    if (usesCleanupOnly(Func))
      Changed |= lowerFunctionCleanupOnly(Func);
    else
      Changed |= lowerFunction(Func, State);
  }

  // After lowering, none of the EH-runtime declarations or the personality
  // function declarations are referenced. Erase the dead ones so the lowered
  // module doesn't drag along external symbols downstream tools would have
  // to model.
  if (Changed) {
    static constexpr StringRef DeadDeclNames[] = {
        "_CxxThrowException",
        "__cxa_throw",
        "__cxa_rethrow",
        "__cxa_allocate_exception",
        "__cxa_free_exception",
        "__cxa_begin_catch",
        "__cxa_end_catch",
        "__CxxFrameHandler3",
        "__gxx_personality_v0",
    };
    SmallVector<Function *, 4> DeadDecls;
    for (Function &F : Mod) {
      if (!F.isDeclaration() || !F.use_empty())
        continue;
      StringRef N = F.getName();
      for (StringRef D : DeadDeclNames) {
        if (N == D) {
          DeadDecls.push_back(&F);
          break;
        }
      }
    }
    for (Function *F : DeadDecls)
      F->eraseFromParent();
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace exclow
