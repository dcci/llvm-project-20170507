//===-- StackProtector.cpp - Stack Protector Insertion --------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass inserts stack protectors into functions which need them. A variable
// with a random value in it is stored onto the stack before the local variables
// are allocated. Upon exiting the block, the stored value is checked. If it's
// changed, then there was some sort of violation and the program aborts.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "stack-protector"
#include "llvm/CodeGen/Analysis.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Target/TargetLowering.h"
#include <cstdlib>
using namespace llvm;

STATISTIC(NumFunProtected, "Number of functions protected");
STATISTIC(NumAddrTaken, "Number of local variables that have their address"
                        " taken.");

static cl::opt<bool>
EnableSelectionDAGSP("enable-selectiondag-sp", cl::init(true),
                     cl::Hidden);

namespace {
  class StackProtector : public FunctionPass {
    const TargetMachine *TM;

    /// TLI - Keep a pointer of a TargetLowering to consult for determining
    /// target type sizes.
    const TargetLoweringBase *TLI;
    const Triple Trip;

    Function *F;
    Module *M;

    DominatorTree *DT;

    /// \brief The minimum size of buffers that will receive stack smashing
    /// protection when -fstack-protection is used.
    unsigned SSPBufferSize;

    /// VisitedPHIs - The set of PHI nodes visited when determining
    /// if a variable's reference has been taken.  This set
    /// is maintained to ensure we don't visit the same PHI node multiple
    /// times.
    SmallPtrSet<const PHINode*, 16> VisitedPHIs;

    /// InsertStackProtectors - Insert code into the prologue and epilogue of
    /// the function.
    ///
    ///  - The prologue code loads and stores the stack guard onto the stack.
    ///  - The epilogue checks the value stored in the prologue against the
    ///    original value. It calls __stack_chk_fail if they differ.
    bool InsertStackProtectors();

    /// CreateFailBB - Create a basic block to jump to when the stack protector
    /// check fails.
    BasicBlock *CreateFailBB();

    /// ContainsProtectableArray - Check whether the type either is an array or
    /// contains an array of sufficient size so that we need stack protectors
    /// for it.
    bool ContainsProtectableArray(Type *Ty, bool Strong = false,
                                  bool InStruct = false) const;

    /// \brief Check whether a stack allocation has its address taken.
    bool HasAddressTaken(const Instruction *AI);

    /// RequiresStackProtector - Check whether or not this function needs a
    /// stack protector based upon the stack protector level.
    bool RequiresStackProtector();
  public:
    static char ID;             // Pass identification, replacement for typeid.
    StackProtector() : FunctionPass(ID), TM(0), TLI(0), SSPBufferSize(0) {
      initializeStackProtectorPass(*PassRegistry::getPassRegistry());
    }
    StackProtector(const TargetMachine *TM)
      : FunctionPass(ID), TM(TM), TLI(0), Trip(TM->getTargetTriple()),
        SSPBufferSize(8) {
      initializeStackProtectorPass(*PassRegistry::getPassRegistry());
    }

    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.addPreserved<DominatorTree>();
    }

    virtual bool runOnFunction(Function &Fn);
  };
} // end anonymous namespace

char StackProtector::ID = 0;
INITIALIZE_PASS(StackProtector, "stack-protector",
                "Insert stack protectors", false, false)

FunctionPass *llvm::createStackProtectorPass(const TargetMachine *TM) {
  return new StackProtector(TM);
}

bool StackProtector::runOnFunction(Function &Fn) {
  F = &Fn;
  M = F->getParent();
  DT = getAnalysisIfAvailable<DominatorTree>();
  TLI = TM->getTargetLowering();

  if (!RequiresStackProtector()) return false;

  Attribute Attr =
    Fn.getAttributes().getAttribute(AttributeSet::FunctionIndex,
                                    "stack-protector-buffer-size");
  if (Attr.isStringAttribute())
    SSPBufferSize = atoi(Attr.getValueAsString().data());

  ++NumFunProtected;
  return InsertStackProtectors();
}

/// ContainsProtectableArray - Check whether the type either is an array or
/// contains a char array of sufficient size so that we need stack protectors
/// for it.
bool StackProtector::ContainsProtectableArray(Type *Ty, bool Strong,
                                              bool InStruct) const {
  if (!Ty) return false;
  if (ArrayType *AT = dyn_cast<ArrayType>(Ty)) {
    // In strong mode any array, regardless of type and size, triggers a
    // protector
    if (Strong)
      return true;
    if (!AT->getElementType()->isIntegerTy(8)) {
      // If we're on a non-Darwin platform or we're inside of a structure, don't
      // add stack protectors unless the array is a character array.
      if (InStruct || !Trip.isOSDarwin())
          return false;
    }

    // If an array has more than SSPBufferSize bytes of allocated space, then we
    // emit stack protectors.
    if (SSPBufferSize <= TLI->getDataLayout()->getTypeAllocSize(AT))
      return true;
  }

  const StructType *ST = dyn_cast<StructType>(Ty);
  if (!ST) return false;

  for (StructType::element_iterator I = ST->element_begin(),
         E = ST->element_end(); I != E; ++I)
    if (ContainsProtectableArray(*I, Strong, true))
      return true;

  return false;
}

bool StackProtector::HasAddressTaken(const Instruction *AI) {
  for (Value::const_use_iterator UI = AI->use_begin(), UE = AI->use_end();
        UI != UE; ++UI) {
    const User *U = *UI;
    if (const StoreInst *SI = dyn_cast<StoreInst>(U)) {
      if (AI == SI->getValueOperand())
        return true;
    } else if (const PtrToIntInst *SI = dyn_cast<PtrToIntInst>(U)) {
      if (AI == SI->getOperand(0))
        return true;
    } else if (isa<CallInst>(U)) {
      return true;
    } else if (isa<InvokeInst>(U)) {
      return true;
    } else if (const SelectInst *SI = dyn_cast<SelectInst>(U)) {
      if (HasAddressTaken(SI))
        return true;
    } else if (const PHINode *PN = dyn_cast<PHINode>(U)) {
      // Keep track of what PHI nodes we have already visited to ensure
      // they are only visited once.
      if (VisitedPHIs.insert(PN))
        if (HasAddressTaken(PN))
          return true;
    } else if (const GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(U)) {
      if (HasAddressTaken(GEP))
        return true;
    } else if (const BitCastInst *BI = dyn_cast<BitCastInst>(U)) {
      if (HasAddressTaken(BI))
        return true;
    }
  }
  return false;
}

/// \brief Check whether or not this function needs a stack protector based
/// upon the stack protector level.
///
/// We use two heuristics: a standard (ssp) and strong (sspstrong).
/// The standard heuristic which will add a guard variable to functions that
/// call alloca with a either a variable size or a size >= SSPBufferSize,
/// functions with character buffers larger than SSPBufferSize, and functions
/// with aggregates containing character buffers larger than SSPBufferSize. The
/// strong heuristic will add a guard variables to functions that call alloca
/// regardless of size, functions with any buffer regardless of type and size,
/// functions with aggregates that contain any buffer regardless of type and
/// size, and functions that contain stack-based variables that have had their
/// address taken.
bool StackProtector::RequiresStackProtector() {
  bool Strong = false;
  if (F->getAttributes().hasAttribute(AttributeSet::FunctionIndex,
                                      Attribute::StackProtectReq))
    return true;
  else if (F->getAttributes().hasAttribute(AttributeSet::FunctionIndex,
                                           Attribute::StackProtectStrong))
    Strong = true;
  else if (!F->getAttributes().hasAttribute(AttributeSet::FunctionIndex,
                                            Attribute::StackProtect))
    return false;

  for (Function::iterator I = F->begin(), E = F->end(); I != E; ++I) {
    BasicBlock *BB = I;

    for (BasicBlock::iterator
           II = BB->begin(), IE = BB->end(); II != IE; ++II) {
      if (AllocaInst *AI = dyn_cast<AllocaInst>(II)) {
        if (AI->isArrayAllocation()) {
          // SSP-Strong: Enable protectors for any call to alloca, regardless
          // of size.
          if (Strong)
            return true;

          if (const ConstantInt *CI =
               dyn_cast<ConstantInt>(AI->getArraySize())) {
            if (CI->getLimitedValue(SSPBufferSize) >= SSPBufferSize)
              // A call to alloca with size >= SSPBufferSize requires
              // stack protectors.
              return true;
          } else {
            // A call to alloca with a variable size requires protectors.
            return true;
          }
        }

        if (ContainsProtectableArray(AI->getAllocatedType(), Strong))
          return true;

        if (Strong && HasAddressTaken(AI)) {
          ++NumAddrTaken;
          return true;
        }
      }
    }
  }

  return false;
}

static bool InstructionWillNotHaveChain(const Instruction *I) {
  return !I->mayHaveSideEffects() && !I->mayReadFromMemory() &&
    isSafeToSpeculativelyExecute(I);
}

/// Identify if RI has a previous instruction in the "Tail Position" and return
/// it. Otherwise return 0.
///
/// This is based off of the code in llvm::isInTailCallPosition. The difference
/// is that it inverts the first part of llvm::isInTailCallPosition since
/// isInTailCallPosition is checking if a call is in a tail call position, and
/// we are searching for an unknown tail call that might be in the tail call
/// position. Once we find the call though, the code uses the same refactored
/// code, returnTypeIsEligibleForTailCall.
static CallInst *FindPotentialTailCall(BasicBlock *BB, ReturnInst *RI,
                                       const TargetLoweringBase *TLI) {
  // Establish a reasonable upper bound on the maximum amount of instructions we
  // will look through to find a tail call.
  unsigned SearchCounter = 0;
  const unsigned MaxSearch = 4;
  bool NoInterposingChain = true;

  for (BasicBlock::reverse_iterator I = llvm::next(BB->rbegin()), E = BB->rend();
       I != E && SearchCounter < MaxSearch; ++I) {
    Instruction *Inst = &*I;

    // Skip over debug intrinsics and do not allow them to affect our MaxSearch
    // counter.
    if (isa<DbgInfoIntrinsic>(Inst))
      continue;

    // If we find a call and the following conditions are satisifed, then we
    // have found a tail call that satisfies at least the target independent
    // requirements of a tail call:
    //
    // 1. The call site has the tail marker.
    //
    // 2. The call site either will not cause the creation of a chain or if a
    // chain is necessary there are no instructions in between the callsite and
    // the call which would create an interposing chain.
    //
    // 3. The return type of the function does not impede tail call
    // optimization.
    if (CallInst *CI = dyn_cast<CallInst>(Inst)) {
      if (CI->isTailCall() &&
          (InstructionWillNotHaveChain(CI) || NoInterposingChain) &&
          returnTypeIsEligibleForTailCall(BB->getParent(), CI, RI, *TLI))
        return CI;
    }

    // If we did not find a call see if we have an instruction that may create
    // an interposing chain.
    NoInterposingChain = NoInterposingChain && InstructionWillNotHaveChain(Inst);

    // Increment max search.
    SearchCounter++;
  }

  return 0;
}

/// Insert code into the entry block that stores the __stack_chk_guard
/// variable onto the stack:
///
///   entry:
///     StackGuardSlot = alloca i8*
///     StackGuard = load __stack_chk_guard
///     call void @llvm.stackprotect.create(StackGuard, StackGuardSlot)
///
/// Returns true if the platform/triple supports the stackprotectorcreate pseudo
/// node.
static bool CreatePrologue(Function *F, Module *M, ReturnInst *RI,
                           const TargetLoweringBase *TLI, const Triple &Trip,
                           AllocaInst *&AI, Value *&StackGuardVar) {
  bool SupportsSelectionDAGSP = false;
  PointerType *PtrTy = Type::getInt8PtrTy(RI->getContext());
  unsigned AddressSpace, Offset;
  if (TLI->getStackCookieLocation(AddressSpace, Offset)) {
    Constant *OffsetVal =
      ConstantInt::get(Type::getInt32Ty(RI->getContext()), Offset);

    StackGuardVar = ConstantExpr::getIntToPtr(OffsetVal,
                                              PointerType::get(PtrTy,
                                                               AddressSpace));
  } else if (Trip.getOS() == llvm::Triple::OpenBSD) {
    StackGuardVar = M->getOrInsertGlobal("__guard_local", PtrTy);
    cast<GlobalValue>(StackGuardVar)
      ->setVisibility(GlobalValue::HiddenVisibility);
  } else {
    SupportsSelectionDAGSP = true;
    StackGuardVar = M->getOrInsertGlobal("__stack_chk_guard", PtrTy);
  }

  BasicBlock &Entry = F->getEntryBlock();
  Instruction *InsPt = &Entry.front();

  AI = new AllocaInst(PtrTy, "StackGuardSlot", InsPt);
  LoadInst *LI = new LoadInst(StackGuardVar, "StackGuard", false, InsPt);

  Value *Args[] = { LI, AI };
  CallInst::
    Create(Intrinsic::getDeclaration(M, Intrinsic::stackprotector),
           Args, "", InsPt);

  return SupportsSelectionDAGSP;
}

/// InsertStackProtectors - Insert code into the prologue and epilogue of the
/// function.
///
///  - The prologue code loads and stores the stack guard onto the stack.
///  - The epilogue checks the value stored in the prologue against the original
///    value. It calls __stack_chk_fail if they differ.
bool StackProtector::InsertStackProtectors() {
  bool HasPrologue = false;
  bool SupportsSelectionDAGSP =
    EnableSelectionDAGSP && !TM->Options.EnableFastISel;
  AllocaInst *AI = 0;           // Place on stack that stores the stack guard.
  Value *StackGuardVar = 0;     // The stack guard variable.

  for (Function::iterator I = F->begin(), E = F->end(); I != E; ) {
    BasicBlock *BB = I++;
    ReturnInst *RI = dyn_cast<ReturnInst>(BB->getTerminator());
    if (!RI)
      continue;

    if (!HasPrologue) {
      HasPrologue = true;
      SupportsSelectionDAGSP &= CreatePrologue(F, M, RI, TLI, Trip, AI,
                                               StackGuardVar);
    }

    if (SupportsSelectionDAGSP) {
      // Since we have a potential tail call, insert the special stack check
      // intrinsic.
      Instruction *InsertionPt = 0;
      if (CallInst *CI = FindPotentialTailCall(BB, RI, TLI)) {
        InsertionPt = CI;
      } else {
        InsertionPt = RI;
        // At this point we know that BB has a return statement so it *DOES*
        // have a terminator.
        assert(InsertionPt != 0 && "BB must have a terminator instruction at "
               "this point.");
      }

      Function *Intrinsic =
        Intrinsic::getDeclaration(M, Intrinsic::stackprotectorcheck);
      Value *Args[] = { StackGuardVar };
      CallInst::Create(Intrinsic, Args, "", InsertionPt);

    } else {
      // If we do not support SelectionDAG based tail calls, generate IR level
      // tail calls.
      //
      // For each block with a return instruction, convert this:
      //
      //   return:
      //     ...
      //     ret ...
      //
      // into this:
      //
      //   return:
      //     ...
      //     %1 = load __stack_chk_guard
      //     %2 = load StackGuardSlot
      //     %3 = cmp i1 %1, %2
      //     br i1 %3, label %SP_return, label %CallStackCheckFailBlk
      //
      //   SP_return:
      //     ret ...
      //
      //   CallStackCheckFailBlk:
      //     call void @__stack_chk_fail()
      //     unreachable

      // Create the FailBB. We duplicate the BB every time since the MI tail
      // merge pass will merge together all of the various BB into one including
      // fail BB generated by the stack protector pseudo instruction.
      BasicBlock *FailBB = CreateFailBB();

      // Split the basic block before the return instruction.
      BasicBlock *NewBB = BB->splitBasicBlock(RI, "SP_return");

      // Update the dominator tree if we need to.
      if (DT && DT->isReachableFromEntry(BB)) {
        DT->addNewBlock(NewBB, BB);
        DT->addNewBlock(FailBB, BB);
      }

      // Remove default branch instruction to the new BB.
      BB->getTerminator()->eraseFromParent();

      // Move the newly created basic block to the point right after the old
      // basic block so that it's in the "fall through" position.
      NewBB->moveAfter(BB);

      // Generate the stack protector instructions in the old basic block.
      LoadInst *LI1 = new LoadInst(StackGuardVar, "", false, BB);
      LoadInst *LI2 = new LoadInst(AI, "", true, BB);
      ICmpInst *Cmp = new ICmpInst(*BB, CmpInst::ICMP_EQ, LI1, LI2, "");
      BranchInst::Create(NewBB, FailBB, Cmp, BB);
    }
  }

  // Return if we didn't modify any basic blocks. I.e., there are no return
  // statements in the function.
  if (!HasPrologue)
    return false;

  return true;
}

/// CreateFailBB - Create a basic block to jump to when the stack protector
/// check fails.
BasicBlock *StackProtector::CreateFailBB() {
  LLVMContext &Context = F->getContext();
  BasicBlock *FailBB = BasicBlock::Create(Context, "CallStackCheckFailBlk", F);
  if (Trip.getOS() == llvm::Triple::OpenBSD) {
    Constant *StackChkFail = M->getOrInsertFunction(
        "__stack_smash_handler", Type::getVoidTy(Context),
        Type::getInt8PtrTy(Context), NULL);

    Constant *NameStr = ConstantDataArray::getString(Context, F->getName());
    Constant *FuncName =
        new GlobalVariable(*M, NameStr->getType(), true,
                           GlobalVariable::PrivateLinkage, NameStr, "SSH");

    SmallVector<Constant *, 2> IdxList;
    IdxList.push_back(ConstantInt::get(Type::getInt8Ty(Context), 0));
    IdxList.push_back(ConstantInt::get(Type::getInt8Ty(Context), 0));

    SmallVector<Value *, 1> Args;
    Args.push_back(ConstantExpr::getGetElementPtr(FuncName, IdxList));

    CallInst::Create(StackChkFail, Args, "", FailBB);
  } else {
    Constant *StackChkFail = M->getOrInsertFunction(
        "__stack_chk_fail", Type::getVoidTy(Context), NULL);
    CallInst::Create(StackChkFail, "", FailBB);
  }
  new UnreachableInst(Context, FailBB);
  return FailBB;
}
