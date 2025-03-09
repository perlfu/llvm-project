//===-- AMDGPUWholeWaveMode.cpp - enter and suspend whole wave modes ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//

#include "AMDGPUWholeWaveMode.h"
#include "AMDGPU.h"
#include "AMDGPULaneMaskUtils.h"
#include "GCNSubtarget.h"
#include "MCTargetDesc/AMDGPUMCTargetDesc.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "amdgpu-wwm"

namespace {

enum {
  StateNonStrict = 0x1,
  StateStrictWWM = 0x2,
  StateStrictWQM = 0x4,
  StateStrict = StateStrictWWM | StateStrictWQM,
};

struct PrintState {
public:
  int State;

  explicit PrintState(int State) : State(State) {}
};

#ifndef NDEBUG
static raw_ostream &operator<<(raw_ostream &OS, const PrintState &PS) {

  static const std::pair<char, const char *> Mapping[] = {
      std::pair(StateNonStrict, "Default"),
      std::pair(StateStrictWWM, "StrictWWM"),
      std::pair(StateStrictWQM, "StrictWQM")};
  char State = PS.State;
  for (auto M : Mapping) {
    if (State & M.first) {
      OS << M.second;
      State &= ~M.first;

      if (State)
        OS << '|';
    }
  }
  assert(State == 0);
  return OS;
}
#endif

struct InstrInfo {
  char Needs = 0;
  char Disabled = 0;
  char MarkedStates = 0;
};

struct BlockInfo {
  char Needs = 0;
};

using WorkItem = MachineInstr *;

class AMDGPUWholeWaveMode {
public:
  AMDGPUWholeWaveMode(MachineFunction &MF, LiveIntervals *LIS)
      : ST(&MF.getSubtarget<GCNSubtarget>()), TII(ST->getInstrInfo()),
        TRI(&TII->getRegisterInfo()), MRI(&MF.getRegInfo()), LIS(LIS),
        LMC(AMDGPU::LaneMaskConstants::get(*ST)) {}
  bool run(MachineFunction &MF);

private:
  const GCNSubtarget *ST;
  const SIInstrInfo *TII;
  const SIRegisterInfo *TRI;
  MachineRegisterInfo *MRI;
  LiveIntervals *LIS;
  const AMDGPU::LaneMaskConstants &LMC;

  const TargetRegisterClass *BoolRC;

  DenseMap<const MachineInstr *, InstrInfo> Instructions;
  MapVector<MachineBasicBlock *, BlockInfo> Blocks;

  SmallVector<MachineInstr *, 4> LowerToMovInstrs;
  SmallSetVector<MachineInstr *, 4> LowerToCopyInstrs;
  SmallVector<MachineInstr *, 4> InitExecInstrs;
  SmallVector<MachineInstr *, 4> SetInactiveInstrs;

  void printInfo();

  void markInstruction(MachineInstr &MI, char Flag,
                       std::vector<WorkItem> &Worklist);
  void markDefs(const MachineInstr &UseMI, LiveRange &LR, Register Reg,
                unsigned SubReg, char Flag, std::vector<WorkItem> &Worklist);
  void markOperand(const MachineInstr &MI, const MachineOperand &Op, char Flag,
                   std::vector<WorkItem> &Worklist);
  void markInstructionUses(const MachineInstr &MI, char Flag,
                           std::vector<WorkItem> &Worklist);
  char scanInstructions(MachineFunction &MF, std::vector<WorkItem> &Worklist);
  void propagateInstruction(MachineInstr &MI, std::vector<WorkItem> &Worklist);
  char analyzeFunction(MachineFunction &MF);

  MachineBasicBlock::iterator saveSCC(MachineBasicBlock &MBB,
                                      MachineBasicBlock::iterator Before);
  MachineBasicBlock::iterator
  prepareInsertion(MachineBasicBlock &MBB, MachineBasicBlock::iterator First,
                   MachineBasicBlock::iterator Last, bool PreferLast,
                   bool SaveSCC);
  void toStrictMode(MachineBasicBlock &MBB, MachineBasicBlock::iterator Before,
                    Register SaveOrig, char StrictStateNeeded);
  void fromStrictMode(MachineBasicBlock &MBB,
                      MachineBasicBlock::iterator Before, Register SavedOrig,
                      char CurrentStrictState);

  void processBlock(MachineBasicBlock &MBB, BlockInfo &BI, bool IsEntry);

  bool lowerMovInstrs();
  bool lowerCopyInstrs();
  void lowerInitExec(MachineInstr &MI);
  bool lowerInitExecInstrs();
};

class AMDGPUWholeWaveModeLegacy : public MachineFunctionPass {
public:
  static char ID;

  AMDGPUWholeWaveModeLegacy() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override { return "AMDGPU Whole Wave Mode"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LiveIntervalsWrapperPass>();
    AU.addPreserved<SlotIndexesWrapperPass>();
    AU.addPreserved<LiveIntervalsWrapperPass>();
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};
} // end anonymous namespace

char AMDGPUWholeWaveModeLegacy::ID = 0;
char &llvm::AMDGPUWholeWaveModeID = AMDGPUWholeWaveModeLegacy::ID;

INITIALIZE_PASS_BEGIN(AMDGPUWholeWaveModeLegacy, DEBUG_TYPE,
                      "AMDGPU Whole Wave Mode", false, false)
INITIALIZE_PASS_DEPENDENCY(LiveIntervalsWrapperPass)
INITIALIZE_PASS_END(AMDGPUWholeWaveModeLegacy, DEBUG_TYPE,
                    "AMDGPU Whole Wave Mode", false, false)

FunctionPass *llvm::createAMDGPUWholeWaveModeLegacyPass() {
  return new AMDGPUWholeWaveModeLegacy;
}

#ifndef NDEBUG
LLVM_DUMP_METHOD void AMDGPUWholeWaveMode::printInfo() {
  for (const auto &BII : Blocks) {
    dbgs() << "\n"
           << printMBBReference(*BII.first) << ":\n"
           << ", Needs = " << PrintState(BII.second.Needs) << "\n\n";

    for (const MachineInstr &MI : *BII.first) {
      auto III = Instructions.find(&MI);
      if (III != Instructions.end()) {
        dbgs() << "  " << MI << "    Needs = " << PrintState(III->second.Needs)
               << '\n';
      }
    }
  }
}
#endif

void AMDGPUWholeWaveMode::markInstruction(MachineInstr &MI, char Flag,
                                          std::vector<WorkItem> &Worklist) {
  InstrInfo &II = Instructions[&MI];

  // Capture all states requested in marking including disabled ones.
  II.MarkedStates |= Flag;

  // Remove any disabled states from the flag. The user that required it gets
  // an undefined value in the helper lanes. For example, this can happen if
  // the result of an atomic is used by instruction that requires WQM, where
  // ignoring the request for WQM is correct as per the relevant specs.
  Flag &= ~II.Disabled;

  // Ignore if the flag is already encompassed by the existing needs, or we
  // just disabled everything.
  if ((II.Needs & Flag) == Flag)
    return;

  LLVM_DEBUG(dbgs() << "markInstruction " << PrintState(Flag) << ": " << MI);
  II.Needs |= Flag;
  Worklist.emplace_back(&MI);
}

/// Mark all relevant definitions of register \p Reg in usage \p UseMI.
void AMDGPUWholeWaveMode::markDefs(const MachineInstr &UseMI, LiveRange &LR,
                                   Register Reg, unsigned SubReg, char Flag,
                                   std::vector<WorkItem> &Worklist) {
  LLVM_DEBUG(dbgs() << "markDefs " << PrintState(Flag) << ": " << UseMI);

  LiveQueryResult UseLRQ = LR.Query(LIS->getInstructionIndex(UseMI));
  const VNInfo *Value = UseLRQ.valueIn();
  if (!Value)
    return;

  // Note: this code assumes that lane masks on AMDGPU completely
  // cover registers.
  const LaneBitmask UseLanes =
      SubReg ? TRI->getSubRegIndexLaneMask(SubReg)
             : (Reg.isVirtual() ? MRI->getMaxLaneMaskForVReg(Reg)
                                : LaneBitmask::getNone());

  // Perform a depth-first iteration of the LiveRange graph marking defs.
  // Stop processing of a given branch when all use lanes have been defined.
  // The first definition stops processing for a physical register.
  struct PhiEntry {
    const VNInfo *Phi;
    unsigned PredIdx;
    LaneBitmask DefinedLanes;

    PhiEntry(const VNInfo *Phi, unsigned PredIdx, LaneBitmask DefinedLanes)
        : Phi(Phi), PredIdx(PredIdx), DefinedLanes(DefinedLanes) {}
  };
  using VisitKey = std::pair<const VNInfo *, LaneBitmask>;
  SmallVector<PhiEntry, 2> PhiStack;
  SmallSet<VisitKey, 4> Visited;
  LaneBitmask DefinedLanes;
  unsigned NextPredIdx = 0; // Only used for processing phi nodes
  do {
    const VNInfo *NextValue = nullptr;
    const VisitKey Key(Value, DefinedLanes);

    if (Visited.insert(Key).second) {
      // On first visit to a phi then start processing first predecessor
      NextPredIdx = 0;
    }

    if (Value->isPHIDef()) {
      // Each predecessor node in the phi must be processed as a subgraph
      const MachineBasicBlock *MBB = LIS->getMBBFromIndex(Value->def);
      assert(MBB && "Phi-def has no defining MBB");

      // Find next predecessor to process
      unsigned Idx = NextPredIdx;
      const auto *PI = MBB->pred_begin() + Idx;
      const auto *PE = MBB->pred_end();
      for (; PI != PE && !NextValue; ++PI, ++Idx) {
        if (const VNInfo *VN = LR.getVNInfoBefore(LIS->getMBBEndIdx(*PI))) {
          if (!Visited.count(VisitKey(VN, DefinedLanes)))
            NextValue = VN;
        }
      }

      // If there are more predecessors to process; add phi to stack
      if (PI != PE)
        PhiStack.emplace_back(Value, Idx, DefinedLanes);
    } else {
      MachineInstr *MI = LIS->getInstructionFromIndex(Value->def);
      assert(MI && "Def has no defining instruction");

      if (Reg.isVirtual()) {
        // Iterate over all operands to find relevant definitions
        bool HasDef = false;
        for (const MachineOperand &Op : MI->all_defs()) {
          if (Op.getReg() != Reg)
            continue;

          // Compute lanes defined and overlap with use
          LaneBitmask OpLanes =
              Op.isUndef() ? LaneBitmask::getAll()
                           : TRI->getSubRegIndexLaneMask(Op.getSubReg());
          LaneBitmask Overlap = (UseLanes & OpLanes);

          // Record if this instruction defined any of use
          HasDef |= Overlap.any();

          // Mark any lanes defined
          DefinedLanes |= OpLanes;
        }

        // Check if all lanes of use have been defined
        if ((DefinedLanes & UseLanes) != UseLanes) {
          // Definition not complete; need to process input value
          LiveQueryResult LRQ = LR.Query(LIS->getInstructionIndex(*MI));
          if (const VNInfo *VN = LRQ.valueIn()) {
            if (!Visited.count(VisitKey(VN, DefinedLanes)))
              NextValue = VN;
          }
        }

        // Only mark the instruction if it defines some part of the use
        if (HasDef)
          markInstruction(*MI, Flag, Worklist);
      } else {
        // For physical registers simply mark the defining instruction
        markInstruction(*MI, Flag, Worklist);
      }
    }

    if (!NextValue && !PhiStack.empty()) {
      // Reach end of chain; revert to processing last phi
      PhiEntry &Entry = PhiStack.back();
      NextValue = Entry.Phi;
      NextPredIdx = Entry.PredIdx;
      DefinedLanes = Entry.DefinedLanes;
      PhiStack.pop_back();
    }

    Value = NextValue;
  } while (Value);
}

void AMDGPUWholeWaveMode::markOperand(const MachineInstr &MI,
                                      const MachineOperand &Op, char Flag,
                                      std::vector<WorkItem> &Worklist) {
  assert(Op.isReg());
  Register Reg = Op.getReg();

  // Ignore some hardware registers
  switch (Reg) {
  case AMDGPU::EXEC:
  case AMDGPU::EXEC_LO:
    return;
  default:
    break;
  }

  LLVM_DEBUG(dbgs() << "markOperand " << PrintState(Flag) << ": " << Op
                    << " for " << MI);
  if (Reg.isVirtual()) {
    LiveRange &LR = LIS->getInterval(Reg);
    markDefs(MI, LR, Reg, Op.getSubReg(), Flag, Worklist);
  } else {
    // Handle physical registers that we need to track; this is mostly relevant
    // for VCC, which can appear as the (implicit) input of a uniform branch,
    // e.g. when a loop counter is stored in a VGPR.
    for (MCRegUnit Unit : TRI->regunits(Reg.asMCReg())) {
      LiveRange &LR = LIS->getRegUnit(Unit);
      const VNInfo *Value = LR.Query(LIS->getInstructionIndex(MI)).valueIn();
      if (Value)
        markDefs(MI, LR, Unit, AMDGPU::NoSubRegister, Flag, Worklist);
    }
  }
}

/// Mark all instructions defining the uses in \p MI with \p Flag.
void AMDGPUWholeWaveMode::markInstructionUses(const MachineInstr &MI, char Flag,
                                              std::vector<WorkItem> &Worklist) {
  LLVM_DEBUG(dbgs() << "markInstructionUses " << PrintState(Flag) << ": "
                    << MI);

  for (const MachineOperand &Use : MI.all_uses())
    markOperand(MI, Use, Flag, Worklist);
}

char AMDGPUWholeWaveMode::scanInstructions(MachineFunction &MF,
                                           std::vector<WorkItem> &Worklist) {
  char GlobalFlags = 0;

  // We need to visit the basic blocks in reverse post-order so that we visit
  // defs before uses, in particular so that we don't accidentally mark an
  // instruction as needing e.g. WQM before visiting it and realizing it needs
  // WQM disabled.
  ReversePostOrderTraversal<MachineFunction *> RPOT(&MF);
  for (MachineBasicBlock *MBB : RPOT) {
    BlockInfo &BBI = Blocks[MBB];

    for (MachineInstr &MI : *MBB) {
      InstrInfo &III = Instructions[&MI];
      unsigned Opcode = MI.getOpcode();
      char Flags = 0;

      if (Opcode == AMDGPU::STRICT_WWM) {
        markInstructionUses(MI, StateStrictWWM, Worklist);
        GlobalFlags |= StateStrictWWM;
        LowerToMovInstrs.push_back(&MI);
      } else if (Opcode == AMDGPU::STRICT_WQM ||
                 TII->isDualSourceBlendEXP(MI)) {
        // STRICT_WQM is similar to STRICT_WWM, but instead of enabling all
        // threads of the wave like STRICT_WWM, STRICT_WQM enables all threads
        // in quads that have at least one active thread.
        markInstructionUses(MI, StateStrictWQM, Worklist);
        GlobalFlags |= StateStrictWQM;

        if (Opcode == AMDGPU::STRICT_WQM) {
          LowerToMovInstrs.push_back(&MI);
        } else {
          // Dual source blend export acts as implicit strict-wqm, its sources
          // need to be shuffled in strict wqm, but the export itself needs to
          // run in exact mode.
          BBI.Needs |= StateNonStrict;
          GlobalFlags |= StateNonStrict;
          III.Disabled = StateStrict;
        }
      } else if (Opcode == AMDGPU::LDS_PARAM_LOAD ||
                 Opcode == AMDGPU::DS_PARAM_LOAD ||
                 Opcode == AMDGPU::LDS_DIRECT_LOAD ||
                 Opcode == AMDGPU::DS_DIRECT_LOAD) {
        // Mark these STRICTWQM, but only for the instruction, not its operands.
        // This avoid unnecessarily marking M0 as requiring WQM.
        III.Needs |= StateStrictWQM;
        GlobalFlags |= StateStrictWQM;
      } else if (Opcode == AMDGPU::V_SET_INACTIVE_B32) {
        // Disable strict states; StrictWQM will be added as required later.
        III.Disabled = StateStrict;
        MachineOperand &Inactive = MI.getOperand(4);
        if (Inactive.isReg()) {
          if (Inactive.isUndef() && MI.getOperand(3).getImm() == 0)
            LowerToCopyInstrs.insert(&MI);
          else
            markOperand(MI, Inactive, StateStrictWWM, Worklist);
        }
        SetInactiveInstrs.push_back(&MI);
      } else if (TII->isDisableWQM(MI)) {
        // FIXME: does this actually apply to strict modes?
        BBI.Needs |= StateNonStrict;
        GlobalFlags |= StateNonStrict;
        III.Disabled = StateStrict;
      } else if (Opcode == AMDGPU::SI_INIT_EXEC ||
                 Opcode == AMDGPU::SI_INIT_EXEC_FROM_INPUT ||
                 Opcode == AMDGPU::SI_INIT_WHOLE_WAVE) {
        InitExecInstrs.push_back(&MI);
      }

      if (Flags) {
        markInstruction(MI, Flags, Worklist);
        GlobalFlags |= Flags;
      }
    }
  }

  return GlobalFlags;
}

void AMDGPUWholeWaveMode::propagateInstruction(
    MachineInstr &MI, std::vector<WorkItem> &Worklist) {
  MachineBasicBlock *MBB = MI.getParent();
  InstrInfo II =
      Instructions[&MI]; // take a copy to prevent dangling references
  BlockInfo &BI = Blocks[MBB];

  if (II.Needs & StateStrict)
    markInstructionUses(MI, II.Needs, Worklist);

  // Ensure we process a block containing StrictWWM/StrictWQM, even if it does
  // not require any WQM transitions.
  if (II.Needs & StateStrict)
    BI.Needs |= (II.Needs & StateStrict);
}

char AMDGPUWholeWaveMode::analyzeFunction(MachineFunction &MF) {
  std::vector<WorkItem> Worklist;
  char GlobalFlags = scanInstructions(MF, Worklist);

  while (!Worklist.empty()) {
    WorkItem WI = Worklist.back();
    Worklist.pop_back();
    propagateInstruction(*WI, Worklist);
  }

  return GlobalFlags;
}

MachineBasicBlock::iterator
AMDGPUWholeWaveMode::saveSCC(MachineBasicBlock &MBB,
                             MachineBasicBlock::iterator Before) {
  Register SaveReg = MRI->createVirtualRegister(&AMDGPU::SReg_32_XM0RegClass);

  MachineInstr *Save =
      BuildMI(MBB, Before, DebugLoc(), TII->get(AMDGPU::COPY), SaveReg)
          .addReg(AMDGPU::SCC);
  MachineInstr *Restore =
      BuildMI(MBB, Before, DebugLoc(), TII->get(AMDGPU::COPY), AMDGPU::SCC)
          .addReg(SaveReg);

  LIS->InsertMachineInstrInMaps(*Save);
  LIS->InsertMachineInstrInMaps(*Restore);
  LIS->createAndComputeVirtRegInterval(SaveReg);

  return Restore;
}

// Return an iterator in the (inclusive) range [First, Last] at which
// instructions can be safely inserted, keeping in mind that some of the
// instructions we want to add necessarily clobber SCC.
MachineBasicBlock::iterator AMDGPUWholeWaveMode::prepareInsertion(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator First,
    MachineBasicBlock::iterator Last, bool PreferLast, bool SaveSCC) {
  if (!SaveSCC)
    return PreferLast ? Last : First;

  LiveRange &LR =
      LIS->getRegUnit(*TRI->regunits(MCRegister::from(AMDGPU::SCC)).begin());
  auto MBBE = MBB.end();
  SlotIndex FirstIdx = First != MBBE ? LIS->getInstructionIndex(*First)
                                     : LIS->getMBBEndIdx(&MBB);
  SlotIndex LastIdx =
      Last != MBBE ? LIS->getInstructionIndex(*Last) : LIS->getMBBEndIdx(&MBB);
  SlotIndex Idx = PreferLast ? LastIdx : FirstIdx;
  const LiveRange::Segment *S;

  for (;;) {
    S = LR.getSegmentContaining(Idx);
    if (!S)
      break;

    if (PreferLast) {
      SlotIndex Next = S->start.getBaseIndex();
      if (Next < FirstIdx)
        break;
      Idx = Next;
    } else {
      MachineInstr *EndMI = LIS->getInstructionFromIndex(S->end.getBaseIndex());
      assert(EndMI && "Segment does not end on valid instruction");
      auto NextI = std::next(EndMI->getIterator());
      if (NextI == MBB.end())
        break;
      SlotIndex Next = LIS->getInstructionIndex(*NextI);
      if (Next > LastIdx)
        break;
      Idx = Next;
    }
  }

  MachineBasicBlock::iterator MBBI;

  if (MachineInstr *MI = LIS->getInstructionFromIndex(Idx))
    MBBI = MI;
  else {
    assert(Idx == LIS->getMBBEndIdx(&MBB));
    MBBI = MBB.end();
  }

  // Move insertion point past any operations modifying EXEC.
  // This assumes that the value of SCC defined by any of these operations
  // does not need to be preserved.
  while (MBBI != Last) {
    bool IsExecDef = false;
    for (const MachineOperand &MO : MBBI->all_defs()) {
      IsExecDef |=
          MO.getReg() == AMDGPU::EXEC_LO || MO.getReg() == AMDGPU::EXEC;
    }
    if (!IsExecDef)
      break;
    MBBI++;
    S = nullptr;
  }

  if (S)
    MBBI = saveSCC(MBB, MBBI);

  return MBBI;
}

void AMDGPUWholeWaveMode::toStrictMode(MachineBasicBlock &MBB,
                                       MachineBasicBlock::iterator Before,
                                       Register SaveOrig,
                                       char StrictStateNeeded) {
  MachineInstr *MI;
  assert(SaveOrig);
  assert(StrictStateNeeded == StateStrictWWM ||
         StrictStateNeeded == StateStrictWQM);

  if (StrictStateNeeded == StateStrictWWM) {
    MI = BuildMI(MBB, Before, DebugLoc(), TII->get(AMDGPU::ENTER_STRICT_WWM),
                 SaveOrig)
             .addImm(-1);
  } else {
    MI = BuildMI(MBB, Before, DebugLoc(), TII->get(AMDGPU::ENTER_STRICT_WQM),
                 SaveOrig)
             .addImm(-1);
  }
  LIS->InsertMachineInstrInMaps(*MI);
}

void AMDGPUWholeWaveMode::fromStrictMode(MachineBasicBlock &MBB,
                                         MachineBasicBlock::iterator Before,
                                         Register SavedOrig,
                                         char CurrentStrictState) {
  MachineInstr *MI;

  assert(SavedOrig);
  assert(CurrentStrictState == StateStrictWWM ||
         CurrentStrictState == StateStrictWQM);

  if (CurrentStrictState == StateStrictWWM) {
    MI = BuildMI(MBB, Before, DebugLoc(), TII->get(AMDGPU::EXIT_STRICT_WWM),
                 LMC.ExecReg)
             .addReg(SavedOrig);
  } else {
    MI = BuildMI(MBB, Before, DebugLoc(), TII->get(AMDGPU::EXIT_STRICT_WQM),
                 LMC.ExecReg)
             .addReg(SavedOrig);
  }
  LIS->InsertMachineInstrInMaps(*MI);
}

void AMDGPUWholeWaveMode::processBlock(MachineBasicBlock &MBB, BlockInfo &BI,
                                       bool IsEntry) {
  if (!(BI.Needs & StateStrict))
    return;

  LLVM_DEBUG(dbgs() << "\nProcessing block " << printMBBReference(MBB)
                    << ":\n");

  // Block never starts in strict mode.
  char State = StateNonStrict;

  // Register used to hold non-strict EXEC during strict mode.
  Register SavedNonStrictReg = 0;

  auto II = MBB.getFirstNonPHI(), IE = MBB.end();

  // Track first insertion point for mode switches to/from strict.
  MachineBasicBlock::iterator TransitionI = IE;

  for (;;) {
    MachineBasicBlock::iterator Next = II;
    char Valid = StateNonStrict; // Strict mode is disabled by default.

    if (TransitionI == IE)
      TransitionI = II;

    // First, figure out the allowed states (Needs) based on the propagated
    // flags.
    if (II != IE) {
      MachineInstr &MI = *II;

      if (MI.isTerminator() || TII->mayReadEXEC(*MRI, MI)) {
        auto III = Instructions.find(&MI);
        if (III != Instructions.end()) {
          if (III->second.Needs & StateStrictWWM)
            Valid = StateStrictWWM;
          else if (III->second.Needs & StateStrictWQM)
            Valid = StateStrictWQM;
          else
            Valid &= ~III->second.Disabled;
        }
      } else {
        // If the instruction doesn't actually need a correct EXEC, then we can
        // safely leave Strict mode enabled.
        Valid = StateNonStrict | StateStrict;
      }

      // Strict mode exit can occur in terminators, but must be before branches.
      if (MI.isBranch())
        Valid = StateNonStrict;

      ++Next;
    } else {
      // End of basic block; exit Strict mode
      Valid = StateNonStrict;
    }

    // Now, transition if necessary.
    if (!(Valid & State)) {
      // Transition to strict state needs SCC preservation.
      bool SaveSCC = (Valid & StateStrict);
      MachineBasicBlock::iterator Before =
          prepareInsertion(MBB, TransitionI, II, false, SaveSCC);

      // First exit current strict mode.
      if (State & StateStrict) {
        assert(SavedNonStrictReg);
        fromStrictMode(MBB, Before, SavedNonStrictReg, State);
        LIS->createAndComputeVirtRegInterval(SavedNonStrictReg);
        SavedNonStrictReg = 0;
        State = StateNonStrict;
      }
      // Second enter new strict mode.
      if ((Valid & StateStrict) && !(Valid & StateNonStrict)) {
        assert(!SavedNonStrictReg);
        SavedNonStrictReg = MRI->createVirtualRegister(BoolRC);
        toStrictMode(MBB, Before, SavedNonStrictReg, Valid);
        State = Valid;
      }
    }

    // Make V_SET_INACTIVE_B32 aware of active exec mask
    if (II != IE && (II->getOpcode() == AMDGPU::V_SET_INACTIVE_B32) && SavedNonStrictReg) {
      LiveInterval &LI = LIS->getInterval(II->getOperand(5).getReg());
      MRI->constrainRegClass(SavedNonStrictReg, TRI->getWaveMaskRegClass());
      II->getOperand(5).setReg(SavedNonStrictReg);
      LIS->shrinkToUses(&LI);
    }

    // Update transition point
    if (Valid != (StateNonStrict | StateStrict))
      TransitionI = IE;

    if (II == IE)
      break;

    II = Next;
  }
  assert(!SavedNonStrictReg);
}

bool AMDGPUWholeWaveMode::lowerMovInstrs() {
  for (MachineInstr *MI : LowerToMovInstrs) {
    assert(MI->getNumExplicitOperands() == 2);

    const Register Reg = MI->getOperand(0).getReg();

    const TargetRegisterClass *regClass =
        TRI->getRegClassForOperandReg(*MRI, MI->getOperand(0));
    if (TRI->isVGPRClass(regClass)) {
      const unsigned MovOp = TII->getMovOpcode(regClass);
      MI->setDesc(TII->get(MovOp));

      // Check that it already implicitly depends on exec (like all VALU movs
      // should do).
      assert(any_of(MI->implicit_operands(), [](const MachineOperand &MO) {
        return MO.isUse() && MO.getReg() == AMDGPU::EXEC;
      }));
    } else {
      // Remove early-clobber and exec dependency from simple SGPR copies.
      // This allows some to be eliminated during/post RA.
      LLVM_DEBUG(dbgs() << "simplify SGPR copy: " << *MI);
      if (MI->getOperand(0).isEarlyClobber()) {
        LIS->removeInterval(Reg);
        MI->getOperand(0).setIsEarlyClobber(false);
        LIS->createAndComputeVirtRegInterval(Reg);
      }
      int Index = MI->findRegisterUseOperandIdx(AMDGPU::EXEC, /*TRI=*/nullptr);
      while (Index >= 0) {
        MI->removeOperand(Index);
        Index = MI->findRegisterUseOperandIdx(AMDGPU::EXEC, /*TRI=*/nullptr);
      }
      MI->setDesc(TII->get(AMDGPU::COPY));
      LLVM_DEBUG(dbgs() << "  -> " << *MI);
    }
  }
  return !LowerToMovInstrs.empty();
}

bool AMDGPUWholeWaveMode::lowerCopyInstrs() {
  for (MachineInstr *MI : LowerToCopyInstrs) {
    LLVM_DEBUG(dbgs() << "simplify: " << *MI);

    if (MI->getOpcode() == AMDGPU::V_SET_INACTIVE_B32) {
      assert(MI->getNumExplicitOperands() == 6);

      LiveInterval *RecomputeLI = nullptr;
      if (MI->getOperand(4).isReg())
        RecomputeLI = &LIS->getInterval(MI->getOperand(4).getReg());

      MI->removeOperand(5);
      MI->removeOperand(4);
      MI->removeOperand(3);
      MI->removeOperand(1);

      if (RecomputeLI)
        LIS->shrinkToUses(RecomputeLI);
    } else {
      assert(MI->getNumExplicitOperands() == 2);
    }

    unsigned CopyOp = MI->getOperand(1).isReg()
                          ? (unsigned)AMDGPU::COPY
                          : TII->getMovOpcode(TRI->getRegClassForOperandReg(
                                *MRI, MI->getOperand(0)));
    MI->setDesc(TII->get(CopyOp));
    LLVM_DEBUG(dbgs() << " -> " << *MI);
  }
  return !LowerToCopyInstrs.empty();
}

void AMDGPUWholeWaveMode::lowerInitExec(MachineInstr &MI) {
  MachineBasicBlock *MBB = MI.getParent();

  if (MI.getOpcode() == AMDGPU::SI_INIT_WHOLE_WAVE) {
    assert(MBB == &MBB->getParent()->front() &&
           "init whole wave not in entry block");
    Register EntryExec = MRI->createVirtualRegister(TRI->getBoolRC());
    MachineInstr *SaveExec =
        BuildMI(*MBB, MBB->begin(), MI.getDebugLoc(),
                TII->get(LMC.OrSaveExecOpc), EntryExec)
            .addImm(-1);

    // Replace all uses of MI's destination reg with EntryExec.
    MRI->replaceRegWith(MI.getOperand(0).getReg(), EntryExec);

    if (LIS) {
      LIS->RemoveMachineInstrFromMaps(MI);
    }

    MI.eraseFromParent();

    if (LIS) {
      LIS->InsertMachineInstrInMaps(*SaveExec);
      LIS->createAndComputeVirtRegInterval(EntryExec);
    }
    return;
  }

  if (MI.getOpcode() == AMDGPU::SI_INIT_EXEC) {
    // This should be before all vector instructions.
    MachineInstr *InitMI =
        BuildMI(*MBB, MBB->begin(), MI.getDebugLoc(),
                TII->get(LMC.MovOpc),
                LMC.ExecReg)
            .addImm(MI.getOperand(0).getImm());
    if (LIS) {
      LIS->RemoveMachineInstrFromMaps(MI);
      LIS->InsertMachineInstrInMaps(*InitMI);
    }
    MI.eraseFromParent();
    return;
  }

  // Extract the thread count from an SGPR input and set EXEC accordingly.
  // Since BFM can't shift by 64, handle that case with CMP + CMOV.
  //
  // S_BFE_U32 count, input, {shift, 7}
  // S_BFM_B64 exec, count, 0
  // S_CMP_EQ_U32 count, 64
  // S_CMOV_B64 exec, -1
  Register InputReg = MI.getOperand(0).getReg();
  MachineInstr *FirstMI = &*MBB->begin();
  if (InputReg.isVirtual()) {
    MachineInstr *DefInstr = MRI->getVRegDef(InputReg);
    assert(DefInstr && DefInstr->isCopy());
    if (DefInstr->getParent() == MBB) {
      if (DefInstr != FirstMI) {
        // If the `InputReg` is defined in current block, we also need to
        // move that instruction to the beginning of the block.
        DefInstr->removeFromParent();
        MBB->insert(FirstMI, DefInstr);
        if (LIS)
          LIS->handleMove(*DefInstr);
      } else {
        // If first instruction is definition then move pointer after it.
        FirstMI = &*std::next(FirstMI->getIterator());
      }
    }
  }

  // Insert instruction sequence at block beginning (before vector operations).
  const DebugLoc DL = MI.getDebugLoc();
  const unsigned WavefrontSize = ST->getWavefrontSize();
  const unsigned Mask = (WavefrontSize << 1) - 1;
  Register CountReg = MRI->createVirtualRegister(&AMDGPU::SGPR_32RegClass);
  auto BfeMI = BuildMI(*MBB, FirstMI, DL, TII->get(AMDGPU::S_BFE_U32), CountReg)
                   .addReg(InputReg)
                   .addImm((MI.getOperand(1).getImm() & Mask) | 0x70000);
  auto BfmMI =
      BuildMI(*MBB, FirstMI, DL,
              TII->get(LMC.BfmOpc),
              LMC.ExecReg)
          .addReg(CountReg)
          .addImm(0);
  auto CmpMI = BuildMI(*MBB, FirstMI, DL, TII->get(AMDGPU::S_CMP_EQ_U32))
                   .addReg(CountReg, RegState::Kill)
                   .addImm(WavefrontSize);
  auto CmovMI =
      BuildMI(*MBB, FirstMI, DL,
              TII->get(LMC.CMovOpc),
              LMC.ExecReg)
          .addImm(-1);

  if (!LIS) {
    MI.eraseFromParent();
    return;
  }

  LIS->RemoveMachineInstrFromMaps(MI);
  MI.eraseFromParent();

  LIS->InsertMachineInstrInMaps(*BfeMI);
  LIS->InsertMachineInstrInMaps(*BfmMI);
  LIS->InsertMachineInstrInMaps(*CmpMI);
  LIS->InsertMachineInstrInMaps(*CmovMI);

  LIS->removeInterval(InputReg);
  LIS->createAndComputeVirtRegInterval(InputReg);
  LIS->createAndComputeVirtRegInterval(CountReg);
}

bool AMDGPUWholeWaveMode::lowerInitExecInstrs() {
  bool Changed = false;

  for (MachineInstr *MI : InitExecInstrs) {
    lowerInitExec(*MI);
    Changed = true;
  }

  return Changed;
}

bool AMDGPUWholeWaveMode::run(MachineFunction &MF) {
  LLVM_DEBUG(dbgs() << "AMDGPU Whole Wave Mode on " << MF.getName()
                    << " ------------- \n");
  LLVM_DEBUG(MF.dump(););

  Instructions.clear();
  Blocks.clear();
  LowerToMovInstrs.clear();
  LowerToCopyInstrs.clear();
  InitExecInstrs.clear();

  BoolRC = TRI->getBoolRC();

  const char GlobalFlags = analyzeFunction(MF);
  bool Changed = lowerInitExecInstrs();
  MachineBasicBlock &Entry = MF.front();

  // Check if V_SET_INACTIVE was touched by a strict state mode.
  // If so, promote to WWM; otherwise lower to COPY.
  for (MachineInstr *MI : SetInactiveInstrs) {
    if (LowerToCopyInstrs.contains(MI))
      continue;
    auto &Info = Instructions[MI];
    if (Info.MarkedStates & StateStrict) {
      Info.Needs |= StateStrictWWM;
      Info.Disabled &= ~StateStrictWWM;
      Blocks[MI->getParent()].Needs |= StateStrictWWM;
    } else {
      LLVM_DEBUG(dbgs() << "Has no WWM marking: " << *MI);
      LowerToCopyInstrs.insert(MI);
    }
  }

  LLVM_DEBUG(printInfo());

  if (GlobalFlags & StateStrict) {
    for (auto &BII : Blocks)
      processBlock(*BII.first, BII.second, BII.first == &Entry);
    Changed = true;
  }
  Changed |= lowerMovInstrs();
  Changed |= lowerCopyInstrs();

  if (Changed) {
    // Physical registers like SCC aren't tracked by default anyway, so just
    // removing the ranges we computed is the simplest option for maintaining
    // the analysis results.
    LIS->removeAllRegUnitsForPhysReg(AMDGPU::SCC);
    LIS->removeAllRegUnitsForPhysReg(AMDGPU::EXEC);
  }

  return Changed;
}

bool AMDGPUWholeWaveModeLegacy::runOnMachineFunction(MachineFunction &MF) {
  LiveIntervals *LIS = &getAnalysis<LiveIntervalsWrapperPass>().getLIS();
  AMDGPUWholeWaveMode Impl(MF, LIS);
  return Impl.run(MF);
}

PreservedAnalyses
AMDGPUWholeWaveModePass::run(MachineFunction &MF,
                             MachineFunctionAnalysisManager &MFAM) {
  MFPropsModifier _(*this, MF);

  LiveIntervals *LIS = &MFAM.getResult<LiveIntervalsAnalysis>(MF);
  AMDGPUWholeWaveMode Impl(MF, LIS);
  bool Changed = Impl.run(MF);
  if (!Changed)
    return PreservedAnalyses::all();

  PreservedAnalyses PA = getMachineFunctionPassPreservedAnalyses();
  PA.preserve<SlotIndexesAnalysis>();
  PA.preserve<LiveIntervalsAnalysis>();
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
