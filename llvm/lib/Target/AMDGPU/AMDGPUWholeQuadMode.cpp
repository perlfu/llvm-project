//===-- AMDGPUWholeQuadMode.cpp - enter and suspend whole quad mode -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This pass adds instructions to enable whole quad mode for pixel shaders.
/// Whole quad mode enables "helper lanes" which are required for derivative
/// computations.  These helper lanes are non-strict and follow control flow.
///
/// Helper lanes may interfere with shader side effects (stores and atomics)
/// and hence must be temporarily disabled around such instructions.
///
/// An exit from whole quad mode (deactivating helper lanes) is inserted when
/// all operations requiring derivatives have completed.
///
/// This pass is also responsible for pixel shader specific operations which
/// permenantly reduce the set of active lanes: kill and demote.
///
//===----------------------------------------------------------------------===//

#include "AMDGPUWholeQuadMode.h"
#include "AMDGPU.h"
#include "AMDGPUExecMaskAnalysis.h"
#include "AMDGPULaneMaskUtils.h"
#include "GCNSubtarget.h"
#include "MCTargetDesc/AMDGPUMCTargetDesc.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/raw_ostream.h"
#include <queue>

using namespace llvm;

#define DEBUG_TYPE "amdgpu-wqm"

namespace {

enum {
  StateWQM = 0x1,
  StateStrictWWM = 0x2,
  StateStrictWQM = 0x4,
  StateExact = 0x8,
  StateStrict = StateStrictWWM | StateStrictWQM,
  FlagLiveMaskQuery = 0x10,
  FlagKillInstr = 0x20,
};

struct PrintState {
public:
  int State;

  explicit PrintState(int State) : State(State) {}
};

#ifndef NDEBUG
static raw_ostream &operator<<(raw_ostream &OS, const PrintState &PS) {

  static const std::pair<char, const char *> Mapping[] = {
      std::pair(StateWQM, "WQM"), std::pair(StateStrictWWM, "StrictWWM"),
      std::pair(StateStrictWQM, "StrictWQM"), std::pair(StateExact, "Exact")};
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

struct BlockInfo {
  char Flags = 0;
  char ExecMode = 0;
  MachineInstr *LastWQMUser = nullptr;
};

class AMDGPUWholeQuadMode {
public:
  AMDGPUWholeQuadMode(MachineFunction &MF, LiveIntervals *LIS,
                      MachineDominatorTree *MDT, MachinePostDominatorTree *PDT)
      : ST(&MF.getSubtarget<GCNSubtarget>()), TII(ST->getInstrInfo()),
        TRI(&TII->getRegisterInfo()), MRI(&MF.getRegInfo()), LIS(LIS), MDT(MDT),
        PDT(PDT), LMC(AMDGPU::LaneMaskConstants::get(*ST)) {}
  bool run(MachineFunction &MF);

private:
  const GCNSubtarget *ST;
  const SIInstrInfo *TII;
  const SIRegisterInfo *TRI;
  MachineRegisterInfo *MRI;
  LiveIntervals *LIS;
  MachineDominatorTree *MDT;
  MachinePostDominatorTree *PDT;
  const AMDGPU::LaneMaskConstants &LMC;
  AMDGPUExecMaskAnalysis EMA;

  Register LiveMaskReg;

  MapVector<MachineBasicBlock *, BlockInfo> Blocks;

  DenseSet<const MachineBasicBlock *> DivergentByKill;
  SmallSet<const MachineInstr *, 2> WQMExits;

  SmallVector<MachineInstr *, 2> LiveMaskQueries;
  SmallSetVector<MachineInstr *, 4> LowerToCopyInstrs;
  SmallVector<MachineInstr *, 4> KillInstrs;

  void printInfo();
  char analyzeFunction(MachineFunction &MF);

  MachineBasicBlock::iterator saveSCC(MachineBasicBlock &MBB,
                                      MachineBasicBlock::iterator Before);

  bool needsExactMode(const MachineInstr &MI);
  bool isSCCLiveAt(const MachineInstr *MI);
  void insertWQMExit(MachineBasicBlock *MBB);

  MachineBasicBlock *splitBlock(MachineInstr *TermMI,
                                bool UpdateTerminator = true);
  MachineInstr *lowerKillI1(MachineInstr &MI, bool IsWQM);
  MachineInstr *lowerKillF32(MachineInstr &MI);

  void lowerBlock(MachineBasicBlock &MBB, BlockInfo &BI, bool HasEntryExec);

  void lowerCopyInstrs();
  bool lowerKillInstrs(bool IsWQM);
  bool lowerNoWQM(MachineBasicBlock *EntryMBB);
  void lowerLiveMaskQuery(MachineInstr *MI, Register Src);

  MachineBasicBlock::iterator findEntryMI(MachineBasicBlock *EntryMBB);
  void setupLiveMaskReg(MachineBasicBlock *EntryMBB,
                        MachineBasicBlock::iterator InsPt);
};

class AMDGPUWholeQuadModeLegacy : public MachineFunctionPass {
public:
  static char ID;

  AMDGPUWholeQuadModeLegacy() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override { return "AMDGPU Whole Quad Mode"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LiveIntervalsWrapperPass>();
    AU.addRequired<MachinePostDominatorTreeWrapperPass>();
    AU.addPreserved<SlotIndexesWrapperPass>();
    AU.addPreserved<LiveIntervalsWrapperPass>();
    AU.addPreserved<MachineDominatorTreeWrapperPass>();
    AU.addPreserved<MachinePostDominatorTreeWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  MachineFunctionProperties getClearedProperties() const override {
    return MachineFunctionProperties().setIsSSA();
  }
};
} // end anonymous namespace

char AMDGPUWholeQuadModeLegacy::ID = 0;

INITIALIZE_PASS_BEGIN(AMDGPUWholeQuadModeLegacy, DEBUG_TYPE,
                      "AMDGPU Whole Quad Mode", false, false)
INITIALIZE_PASS_DEPENDENCY(LiveIntervalsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachinePostDominatorTreeWrapperPass)
INITIALIZE_PASS_END(AMDGPUWholeQuadModeLegacy, DEBUG_TYPE,
                    "AMDGPU Whole Quad Mode", false, false)

char &llvm::AMDGPUWholeQuadModeID = AMDGPUWholeQuadModeLegacy::ID;

FunctionPass *llvm::createAMDGPUWholeQuadModeLegacyPass() {
  return new AMDGPUWholeQuadModeLegacy;
}

#ifndef NDEBUG
LLVM_DUMP_METHOD void AMDGPUWholeQuadMode::printInfo() {
  // FIXME: make this work again
  for (const auto &BII : Blocks) {
    dbgs() << "\n"
           << printMBBReference(*BII.first) << ":\n"
           << ", Flags = " << PrintState(BII.second.Flags)
           << ", ExecMode = " << BII.second.ExecMode << "\n\n";
  }
}
#endif

bool AMDGPUWholeQuadMode::needsExactMode(const MachineInstr &MI) {
  // Dual source blend export acts as implicit strict-wqm, its sources
  // need to be shuffled in strict wqm, but the export itself needs to
  // run in exact mode.
  return TII->isDisableWQM(MI) || TII->isDualSourceBlendEXP(MI);
}

// Scan instructions to determine which ones require an Exact execmask and
// which ones seed WQM requirements.
char AMDGPUWholeQuadMode::analyzeFunction(MachineFunction &MF) {
  const bool HasImplicitDerivatives =
      MF.getFunction().getCallingConv() == CallingConv::AMDGPU_PS;

  SmallVector<MachineInstr *, 4> SoftWQMInstrs;
  char GlobalFlags = 0;

  for (MachineBasicBlock &MBBI : MF) {
    MachineBasicBlock *MBB = &MBBI;
    BlockInfo &BBI = Blocks[MBB];

    MachineInstr *LastWQMUser = nullptr;
    bool InStrictMode = false;

    for (MachineInstr &MI : *MBB) {
      unsigned Opcode = MI.getOpcode();
      char Flags = 0;

      if (InStrictMode) {
        // Ignore instructions inside strict mode regions
        if (Opcode == AMDGPU::EXIT_STRICT_WWM ||
            Opcode == AMDGPU::EXIT_STRICT_WQM)
          InStrictMode = false;
      } else if (Opcode == AMDGPU::ENTER_STRICT_WWM ||
                 Opcode == AMDGPU::ENTER_STRICT_WQM) {
        // Record strict mode entry in flags so these blocks can be found later
        Flags = Opcode == AMDGPU::ENTER_STRICT_WWM ? StateStrictWWM
                                                   : StateStrictWQM;
        InStrictMode = true;
      } else if (TII->isWQM(Opcode)) {
        // If LOD is not supported then WQM is not needed.
        // Only generate implicit WQM if implicit derivatives are required.
        // This avoids inserting unintended WQM if a shader type without
        // implicit derivatives uses an image sampling instruction.
        if (ST->hasExtendedImageInsts() && HasImplicitDerivatives) {
          // Sampling instructions don't need to produce results for all pixels
          // in a quad, they just require all inputs of a quad to have been
          // computed for derivatives.
          LLVM_DEBUG(dbgs() << "WQM MI:" << MI);
          Flags = StateWQM;
        }
      } else if (Opcode == AMDGPU::WQM) {
        // The WQM intrinsic requires its output to have all the helper lanes
        // correct, so we need it to be in WQM.
        LLVM_DEBUG(dbgs() << "WQM MI:" << MI);
        Flags = StateWQM;
        LowerToCopyInstrs.insert(&MI);
      } else if (Opcode == AMDGPU::SOFT_WQM) {
        LowerToCopyInstrs.insert(&MI);
        SoftWQMInstrs.push_back(&MI);
      } else if (needsExactMode(MI)) {
        Flags = StateExact;
      } else if (Opcode == AMDGPU::SI_PS_LIVE ||
                 Opcode == AMDGPU::SI_LIVE_MASK) {
        LiveMaskQueries.push_back(&MI);
        Flags = FlagLiveMaskQuery;
      } else if (Opcode == AMDGPU::SI_KILL_I1_TERMINATOR ||
                 Opcode == AMDGPU::SI_KILL_F32_COND_IMM_TERMINATOR ||
                 Opcode == AMDGPU::SI_DEMOTE_I1) {
        KillInstrs.push_back(&MI);
        Flags = FlagKillInstr;
      }

      if (Flags & StateWQM)
        LastWQMUser = &MI;

      GlobalFlags |= Flags;
      BBI.Flags |= Flags;
    }

    BBI.LastWQMUser = LastWQMUser;
  }

  // Mark sure that any SOFT_WQM instructions are computed in WQM if WQM is
  // ever used anywhere in the function. This implements the corresponding
  // semantics of @llvm.amdgcn.softwqm.
  if (GlobalFlags & StateWQM) {
    for (MachineInstr *MI : SoftWQMInstrs) {
      BlockInfo &BBI = Blocks[MI->getParent()];
      BBI.Flags |= StateWQM;
      // Update last WQM user pointer.
      if (!BBI.LastWQMUser || LIS->getInstructionIndex(*MI) >
                                  LIS->getInstructionIndex(*BBI.LastWQMUser))
        BBI.LastWQMUser = MI;
    }
  }

  return GlobalFlags;
}

MachineBasicBlock::iterator
AMDGPUWholeQuadMode::saveSCC(MachineBasicBlock &MBB,
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

MachineBasicBlock *AMDGPUWholeQuadMode::splitBlock(MachineInstr *TermMI,
                                                   bool UpdateTerminator) {
  MachineBasicBlock *BB = TermMI->getParent();
  LLVM_DEBUG(dbgs() << "Split block " << printMBBReference(*BB) << " @ "
                    << *TermMI << "\n");

  MachineBasicBlock *SplitBB =
      BB->splitAt(*TermMI, /*UpdateLiveIns*/ true, LIS);

  if (UpdateTerminator) {
    // Convert last instruction in block to a terminator.
    // Note: this only covers the expected patterns
    unsigned NewOpcode = 0;
    switch (TermMI->getOpcode()) {
    case AMDGPU::S_AND_B32:
      NewOpcode = AMDGPU::S_AND_B32_term;
      break;
    case AMDGPU::S_AND_B64:
      NewOpcode = AMDGPU::S_AND_B64_term;
      break;
    case AMDGPU::S_MOV_B32:
      NewOpcode = AMDGPU::S_MOV_B32_term;
      break;
    case AMDGPU::S_MOV_B64:
      NewOpcode = AMDGPU::S_MOV_B64_term;
      break;
    case AMDGPU::S_ANDN2_B32:
      NewOpcode = AMDGPU::S_ANDN2_B32_term;
      break;
    case AMDGPU::S_ANDN2_B64:
      NewOpcode = AMDGPU::S_ANDN2_B64_term;
      break;
    default:
      llvm_unreachable("Unexpected instruction");
    }

    // These terminators fallthrough to the next block, no need to add an
    // unconditional branch to the next block (SplitBB).
    TermMI->setDesc(TII->get(NewOpcode));
  }

  if (SplitBB != BB) {
    // Update dominator trees
    using DomTreeT = DomTreeBase<MachineBasicBlock>;
    SmallVector<DomTreeT::UpdateType, 16> DTUpdates;
    for (MachineBasicBlock *Succ : SplitBB->successors()) {
      DTUpdates.push_back({DomTreeT::Insert, SplitBB, Succ});
      DTUpdates.push_back({DomTreeT::Delete, BB, Succ});
    }
    DTUpdates.push_back({DomTreeT::Insert, BB, SplitBB});
    if (MDT)
      MDT->applyUpdates(DTUpdates);
    if (PDT)
      PDT->applyUpdates(DTUpdates);
  }

  return SplitBB;
}

MachineInstr *AMDGPUWholeQuadMode::lowerKillF32(MachineInstr &MI) {
  assert(LiveMaskReg);

  const DebugLoc &DL = MI.getDebugLoc();
  unsigned Opcode = 0;

  assert(MI.getOperand(0).isReg());

  // Comparison is for live lanes; however here we compute the inverse
  // (killed lanes).  This is because VCMP will always generate 0 bits
  // for inactive lanes so a mask of live lanes would not be correct
  // inside control flow.
  // Invert the comparison by swapping the operands and adjusting
  // the comparison codes.

  switch (MI.getOperand(2).getImm()) {
  case ISD::SETUEQ:
    Opcode = AMDGPU::V_CMP_LG_F32_e64;
    break;
  case ISD::SETUGT:
    Opcode = AMDGPU::V_CMP_GE_F32_e64;
    break;
  case ISD::SETUGE:
    Opcode = AMDGPU::V_CMP_GT_F32_e64;
    break;
  case ISD::SETULT:
    Opcode = AMDGPU::V_CMP_LE_F32_e64;
    break;
  case ISD::SETULE:
    Opcode = AMDGPU::V_CMP_LT_F32_e64;
    break;
  case ISD::SETUNE:
    Opcode = AMDGPU::V_CMP_EQ_F32_e64;
    break;
  case ISD::SETO:
    Opcode = AMDGPU::V_CMP_O_F32_e64;
    break;
  case ISD::SETUO:
    Opcode = AMDGPU::V_CMP_U_F32_e64;
    break;
  case ISD::SETOEQ:
  case ISD::SETEQ:
    Opcode = AMDGPU::V_CMP_NEQ_F32_e64;
    break;
  case ISD::SETOGT:
  case ISD::SETGT:
    Opcode = AMDGPU::V_CMP_NLT_F32_e64;
    break;
  case ISD::SETOGE:
  case ISD::SETGE:
    Opcode = AMDGPU::V_CMP_NLE_F32_e64;
    break;
  case ISD::SETOLT:
  case ISD::SETLT:
    Opcode = AMDGPU::V_CMP_NGT_F32_e64;
    break;
  case ISD::SETOLE:
  case ISD::SETLE:
    Opcode = AMDGPU::V_CMP_NGE_F32_e64;
    break;
  case ISD::SETONE:
  case ISD::SETNE:
    Opcode = AMDGPU::V_CMP_NLG_F32_e64;
    break;
  default:
    llvm_unreachable("invalid ISD:SET cond code");
  }

  MachineBasicBlock &MBB = *MI.getParent();

  // Pick opcode based on comparison type.
  MachineInstr *VcmpMI;
  const MachineOperand &Op0 = MI.getOperand(0);
  const MachineOperand &Op1 = MI.getOperand(1);

  // VCC represents lanes killed.
  if (TRI->isVGPR(*MRI, Op0.getReg())) {
    Opcode = AMDGPU::getVOPe32(Opcode);
    VcmpMI = BuildMI(MBB, &MI, DL, TII->get(Opcode)).add(Op1).add(Op0);
  } else {
    VcmpMI = BuildMI(MBB, &MI, DL, TII->get(Opcode))
                 .addReg(LMC.VccReg, RegState::Define)
                 .addImm(0) // src0 modifiers
                 .add(Op1)
                 .addImm(0) // src1 modifiers
                 .add(Op0)
                 .addImm(0); // omod
  }

  MachineInstr *MaskUpdateMI =
      BuildMI(MBB, MI, DL, TII->get(LMC.AndN2Opc), LiveMaskReg)
          .addReg(LiveMaskReg)
          .addReg(LMC.VccReg);

  // State of SCC represents whether any lanes are live in mask,
  // if SCC is 0 then no lanes will be alive anymore.
  MachineInstr *EarlyTermMI =
      BuildMI(MBB, MI, DL, TII->get(AMDGPU::SI_EARLY_TERMINATE_SCC0));

  MachineInstr *ExecMaskMI = BuildMI(MBB, MI, DL, TII->get(LMC.AndN2Opc), LMC.ExecReg)
                                 .addReg(LMC.ExecReg)
                                 .addReg(LMC.VccReg);

  assert(MBB.succ_size() == 1);

  // Update live intervals
  LIS->ReplaceMachineInstrInMaps(MI, *VcmpMI);
  MBB.remove(&MI);

  LIS->InsertMachineInstrInMaps(*MaskUpdateMI);
  LIS->InsertMachineInstrInMaps(*EarlyTermMI);
  LIS->InsertMachineInstrInMaps(*ExecMaskMI);

  return ExecMaskMI;
}

MachineInstr *AMDGPUWholeQuadMode::lowerKillI1(MachineInstr &MI, bool IsWQM) {
  assert(LiveMaskReg);

  MachineBasicBlock &MBB = *MI.getParent();

  const DebugLoc &DL = MI.getDebugLoc();
  MachineInstr *MaskUpdateMI = nullptr;

  const bool IsDemote = IsWQM && (MI.getOpcode() == AMDGPU::SI_DEMOTE_I1);
  const MachineOperand &Op = MI.getOperand(0);
  int64_t KillVal = MI.getOperand(1).getImm();
  MachineInstr *ComputeKilledMaskMI = nullptr;
  Register CndReg = !Op.isImm() ? Op.getReg() : Register();
  Register TmpReg;

  // Is this a static or dynamic kill?
  if (Op.isImm()) {
    if (Op.getImm() == KillVal) {
      // Static: all active lanes are killed
      // Note: LiveMaskReg can be ExecReg, forcing EXEC = 0 and SCC = 0.
      MaskUpdateMI = BuildMI(MBB, MI, DL, TII->get(LMC.AndN2Opc), LiveMaskReg)
                         .addReg(LiveMaskReg)
                         .addReg(LMC.ExecReg);
    } else {
      // Static: kill does nothing
      bool IsLastTerminator = std::next(MI.getIterator()) == MBB.end();
      if (!IsLastTerminator) {
        LIS->RemoveMachineInstrFromMaps(MI);
      } else {
        assert(MBB.succ_size() == 1 && MI.getOpcode() != AMDGPU::SI_DEMOTE_I1);
        MachineInstr *NewTerm = BuildMI(MBB, MI, DL, TII->get(AMDGPU::S_BRANCH))
                                    .addMBB(*MBB.succ_begin());
        LIS->ReplaceMachineInstrInMaps(MI, *NewTerm);
      }
      MBB.remove(&MI);
      return nullptr;
    }
  } else {
    if (!KillVal) {
      // Op represents live lanes after kill,
      // so exec mask needs to be factored in.
      TmpReg = MRI->createVirtualRegister(TRI->getBoolRC());
      ComputeKilledMaskMI = BuildMI(MBB, MI, DL, TII->get(LMC.AndN2Opc), TmpReg)
                                .addReg(LMC.ExecReg)
                                .add(Op);
      MaskUpdateMI = BuildMI(MBB, MI, DL, TII->get(LMC.AndN2Opc), LiveMaskReg)
                         .addReg(LiveMaskReg)
                         .addReg(TmpReg);
    } else {
      // Op represents lanes to kill
      MaskUpdateMI = BuildMI(MBB, MI, DL, TII->get(LMC.AndN2Opc), LiveMaskReg)
                         .addReg(LiveMaskReg)
                         .add(Op);
    }
  }

  // State of SCC represents whether any lanes are live in mask,
  // if SCC is 0 then no lanes will be alive anymore.
  MachineInstr *EarlyTermMI =
      BuildMI(MBB, MI, DL, TII->get(AMDGPU::SI_EARLY_TERMINATE_SCC0));

  // In the case we got this far some lanes are still live,
  // update EXEC to deactivate lanes as appropriate.
  MachineInstr *WQMMaskMI = nullptr;
  MachineInstr *NewTerm;
  Register LiveMaskWQM;
  if (IsDemote) {
    // Demote - deactivate quads with only helper lanes
    // LiveMaskWQM = S_WQM LiveMask
    // NewExec = S_AND Exec, LiveMaskWQM
    // SurvivorLane = S_CLZ_I32 Exec
    // SurvivorExec = S_BITSET1 Tmp
    // LiveMaskWQM = S_CSELECT SurvivorExec, NewExec
    // Exec = S_AND Exec, LiveMaskWQM
    LiveMaskWQM = MRI->createVirtualRegister(TRI->getBoolRC());
    WQMMaskMI =
        BuildMI(MBB, MI, DL, TII->get(LMC.WQMOpc), LiveMaskWQM).addReg(LiveMaskReg);
    NewTerm = BuildMI(MBB, MI, DL, TII->get(LMC.AndOpc), LMC.ExecReg)
                  .addReg(LMC.ExecReg)
                  .addReg(LiveMaskWQM);
  } else {
    // Kill - deactivate lanes no longer in live mask
    if (Op.isImm()) {
      NewTerm = BuildMI(MBB, &MI, DL, TII->get(LMC.MovOpc), LMC.ExecReg).addImm(0);
    } else if (!IsWQM) {
      NewTerm = BuildMI(MBB, &MI, DL, TII->get(LMC.AndOpc), LMC.ExecReg)
                    .addReg(LMC.ExecReg)
                    .addReg(LiveMaskReg);
    } else {
      unsigned Opcode = KillVal ? LMC.AndN2Opc : LMC.AndOpc;
      NewTerm = BuildMI(MBB, &MI, DL, TII->get(Opcode), LMC.ExecReg)
                    .addReg(LMC.ExecReg)
                    .add(Op);
    }
  }

  // Update live intervals
  LIS->RemoveMachineInstrFromMaps(MI);
  MBB.remove(&MI);
  assert(EarlyTermMI);
  assert(MaskUpdateMI);
  if (ComputeKilledMaskMI)
    LIS->InsertMachineInstrInMaps(*ComputeKilledMaskMI);
  LIS->InsertMachineInstrInMaps(*MaskUpdateMI);
  LIS->InsertMachineInstrInMaps(*EarlyTermMI);
  if (WQMMaskMI)
    LIS->InsertMachineInstrInMaps(*WQMMaskMI);
  LIS->InsertMachineInstrInMaps(*NewTerm);
  if (CndReg) {
    LIS->removeInterval(CndReg);
    LIS->createAndComputeVirtRegInterval(CndReg);
  }
  if (TmpReg)
    LIS->createAndComputeVirtRegInterval(TmpReg);
  if (LiveMaskWQM)
    LIS->createAndComputeVirtRegInterval(LiveMaskWQM);

  return NewTerm;
}

// Replace (or supplement) instructions accessing live mask.
// This can only happen once all the live mask registers have been created
// and the execute state (WQM/StrictWWM/Exact) of instructions is known.
void AMDGPUWholeQuadMode::lowerBlock(MachineBasicBlock &MBB, BlockInfo &BI,
                                     bool HasEntryExec) {
  LLVM_DEBUG(dbgs() << "\nLowering block " << printMBBReference(MBB) << ":\n");

  const bool DivergesByKill = DivergentByKill.contains(&MBB);
  SmallVector<MachineInstr *, 4> SplitPoints;
  char BlockMode = BI.ExecMode == StateWQM ? StateWQM : StateExact;
  char State = BlockMode;
  Register SavedWQMReg = Register();

  SmallVector<MachineInstr *> ExactEntry;
  SmallVector<MachineInstr *> ExactExit;

  for (MachineInstr &MI : llvm::make_early_inc_range(
           llvm::make_range(MBB.getFirstNonPHI(), MBB.end()))) {
    // Pick up any WQM exits
    if (WQMExits.contains(&MI))
      State = BlockMode = StateExact;

    if (needsExactMode(MI)) {
      if (State == StateExact || State == StateStrict)
        continue; // Already in Exact mode

      MachineBasicBlock::iterator I = MI.getIterator();
      if (isSCCLiveAt(&MI))
        I = saveSCC(MBB, I);

      // Exit WQM (temporarily)
      SavedWQMReg = MRI->createVirtualRegister(TRI->getBoolRC());
      MachineInstr *SaveExecMI =
          BuildMI(MBB, I, DebugLoc(), TII->get(AMDGPU::COPY), SavedWQMReg)
              .addReg(LMC.ExecReg);
      MachineInstr *ExitMI =
          BuildMI(MBB, I, DebugLoc(), TII->get(LMC.AndOpc), LMC.ExecReg)
              .addReg(LMC.ExecReg)
              .addReg(LiveMaskReg);
      LIS->InsertMachineInstrInMaps(*SaveExecMI);
      LIS->InsertMachineInstrInMaps(*ExitMI);
      State = StateExact;
      ExactEntry.push_back(ExitMI);

      continue;
    } else if (BlockMode == StateWQM && State == StateExact) {
      // Restore WQM
      assert(SavedWQMReg);
      MachineInstr *RestoreMI = BuildMI(MBB, MI.getIterator(), DebugLoc(),
                                        TII->get(AMDGPU::COPY), LMC.ExecReg)
                                    .addReg(SavedWQMReg);
      LIS->InsertMachineInstrInMaps(*RestoreMI);
      LIS->createAndComputeVirtRegInterval(SavedWQMReg);
      SavedWQMReg = Register();
      State = StateWQM;
      ExactExit.push_back(RestoreMI);
    }

    MachineInstr *SplitPoint = nullptr;
    switch (MI.getOpcode()) {
    case AMDGPU::SI_DEMOTE_I1:
    case AMDGPU::SI_KILL_I1_TERMINATOR:
      SplitPoint = lowerKillI1(MI, State == StateWQM);
      break;
    case AMDGPU::SI_KILL_F32_COND_IMM_TERMINATOR:
      SplitPoint = lowerKillF32(MI);
      break;
    case AMDGPU::SI_PS_LIVE:
    case AMDGPU::SI_LIVE_MASK:
      lowerLiveMaskQuery(&MI, State == StateWQM ? LiveMaskReg : LMC.ExecReg);
      break;
    case AMDGPU::ENTER_STRICT_WWM:
    case AMDGPU::EXIT_STRICT_WWM:
      State =
          MI.getOpcode() == AMDGPU::ENTER_STRICT_WWM ? StateStrict : BlockMode;
      break;
    case AMDGPU::ENTER_STRICT_WQM:
    case AMDGPU::EXIT_STRICT_WQM:
      State =
          MI.getOpcode() == AMDGPU::ENTER_STRICT_WQM ? StateStrict : BlockMode;
      if (HasEntryExec && BlockMode == StateWQM && !DivergesByKill) {
        // No need for strict transitions within WQM and unmodified Exec
        LIS->RemoveMachineInstrFromMaps(MI);
        MI.eraseFromParent();
      }
      break;
    default:
      break;
    }
    if (SplitPoint)
      SplitPoints.push_back(SplitPoint);
  }

  // Restore WQM if ended the block on a run of exact instructions
  if (BlockMode == StateWQM && State != StateWQM) {
    assert(SavedWQMReg);
    MachineInstr *RestoreMI =
        BuildMI(MBB, MBB.end(), DebugLoc(), TII->get(AMDGPU::COPY), LMC.ExecReg)
            .addReg(SavedWQMReg);
    LIS->InsertMachineInstrInMaps(*RestoreMI);
    LIS->createAndComputeVirtRegInterval(SavedWQMReg);
    State = StateWQM;
    ExactExit.push_back(RestoreMI);
  }

  // Exact regions in non-uniform control flow may yield EXECZ.
  // This must be avoided by branching over them.
  assert(ExactEntry.size() == ExactExit.size());
  if (!HasEntryExec) {
    using DomTreeT = DomTreeBase<MachineBasicBlock>;
    for (unsigned Region = 0; Region < ExactEntry.size(); ++Region) {
      MachineInstr *Entry = ExactEntry[Region];
      MachineInstr *Exit = ExactExit[Region];
      auto *ExitBB = splitBlock(&*std::prev(Exit->getIterator()),
                                /*UpdateTerminator=*/false);
      splitBlock(Entry);

      auto *EntryBB = Entry->getParent();
      assert(ExitBB != EntryBB);
      MachineInstr *BranchMI = BuildMI(*EntryBB, EntryBB->end(), DebugLoc(),
                                       TII->get(AMDGPU::S_CBRANCH_EXECZ))
                                   .addMBB(ExitBB);
      EntryBB->addSuccessor(ExitBB);

      LIS->InsertMachineInstrInMaps(*BranchMI);
      SmallVector<DomTreeT::UpdateType, 1> DTUpdates;
      DTUpdates.push_back({DomTreeT::Insert, EntryBB, ExitBB});
      if (MDT)
        MDT->applyUpdates(DTUpdates);
      if (PDT)
        PDT->applyUpdates(DTUpdates);
    }
  }

  // Perform splitting after instruction scan to simplify iteration.
  for (MachineInstr *MI : SplitPoints)
    splitBlock(MI);
}

void AMDGPUWholeQuadMode::lowerLiveMaskQuery(MachineInstr *MI, Register Src) {
  const DebugLoc &DL = MI->getDebugLoc();
  Register Dest = MI->getOperand(0).getReg();

  MachineInstr *Copy =
      BuildMI(*MI->getParent(), MI, DL, TII->get(AMDGPU::COPY), Dest)
          .addReg(Src);

  LIS->ReplaceMachineInstrInMaps(*MI, *Copy);
  MI->eraseFromParent();
}

void AMDGPUWholeQuadMode::lowerCopyInstrs() {
  for (MachineInstr *MI : LowerToCopyInstrs) {
    LLVM_DEBUG(dbgs() << "simplify: " << *MI);

    assert(MI->getNumExplicitOperands() == 2);
    unsigned CopyOp = MI->getOperand(1).isReg()
                          ? (unsigned)AMDGPU::COPY
                          : TII->getMovOpcode(TRI->getRegClassForOperandReg(
                                *MRI, MI->getOperand(0)));
    MI->setDesc(TII->get(CopyOp));
    LLVM_DEBUG(dbgs() << " -> " << *MI);
  }
}

bool AMDGPUWholeQuadMode::isSCCLiveAt(const MachineInstr *MI) {
  LiveRange &LR =
      LIS->getRegUnit(*TRI->regunits(MCRegister::from(AMDGPU::SCC)).begin());
  return LR.liveAt(LIS->getInstructionIndex(*MI));
}

void AMDGPUWholeQuadMode::insertWQMExit(MachineBasicBlock *MBB) {
  BlockInfo &BBI = Blocks[MBB];
  MachineBasicBlock::iterator I;
  if (BBI.ExecMode != StateWQM) {
    // Block does not expect WQM; exit at start.
    // Note: SCC should not be live at block start so no need to consider it.
    I = MBB->instr_begin() == MBB->instr_end()
            ? MBB->instr_begin()
            : MBB->SkipPHIsLabelsAndDebug(MBB->instr_begin());
  } else {
    assert(BBI.LastWQMUser);
    I = BBI.LastWQMUser->getIterator();
    // As an optimization we can move the exit point before any independent
    // image operations that use implicit derivatives.
    SmallVector<Register> DepRegs;
    while (I != MBB->instr_begin()) {
      unsigned Opcode = I->getOpcode();
      if (!TII->isWQM(Opcode))
        break;
      if (!TII->isMIMG(Opcode) && !TII->isVSAMPLE(Opcode))
        break;
      if (llvm::any_of(DepRegs, [&](Register Reg) {
            return I->modifiesRegister(Reg, TRI);
          }))
        break;
      DepRegs.push_back(I->getOperand(0).getReg());
      I--;
    }
    I++;
    if (isSCCLiveAt(&*I))
      I = saveSCC(*MBB, I);
  }

  MachineInstr *MI = BuildMI(*MBB, I, DebugLoc(), TII->get(LMC.AndOpc), LMC.ExecReg)
                         .addReg(LMC.ExecReg)
                         .addReg(LiveMaskReg);
  LIS->InsertMachineInstrInMaps(*MI);
  WQMExits.insert(MI);
}

bool AMDGPUWholeQuadMode::lowerNoWQM(MachineBasicBlock *EntryMBB) {
  // Only need a dedicated live mask register if exec is manipulated.
  if (KillInstrs.size()) {
    auto EntryMI = findEntryMI(EntryMBB);
    setupLiveMaskReg(EntryMBB, EntryMI);
  } else {
    LiveMaskReg = LMC.ExecReg;
  }
  lowerCopyInstrs();
  for (MachineInstr *MI : LiveMaskQueries)
    lowerLiveMaskQuery(MI, LMC.ExecReg);
  for (MachineInstr *MI : KillInstrs) {
    MachineInstr *SplitPoint = nullptr;
    switch (MI->getOpcode()) {
    case AMDGPU::SI_DEMOTE_I1:
    case AMDGPU::SI_KILL_I1_TERMINATOR:
      SplitPoint = lowerKillI1(*MI, false);
      break;
    case AMDGPU::SI_KILL_F32_COND_IMM_TERMINATOR:
      SplitPoint = lowerKillF32(*MI);
      break;
    }
    if (SplitPoint)
      splitBlock(SplitPoint);
  }
  if (KillInstrs.size()) {
    LIS->createAndComputeVirtRegInterval(LiveMaskReg);
    LIS->removeAllRegUnitsForPhysReg(AMDGPU::SCC);
    LIS->removeAllRegUnitsForPhysReg(AMDGPU::EXEC);
  }
  return LowerToCopyInstrs.size() || LiveMaskQueries.size() ||
         KillInstrs.size();
}

MachineBasicBlock::iterator
AMDGPUWholeQuadMode::findEntryMI(MachineBasicBlock *EntryMBB) {
  MachineBasicBlock::iterator EntryMI = EntryMBB->getFirstNonPHI();
  while (EntryMI != EntryMBB->instr_end() &&
         EntryMI->modifiesRegister(LMC.ExecReg, TRI)) {
    bool Handled = false;
    switch (EntryMI->getOpcode()) {
    case AMDGPU::SI_KILL_I1_TERMINATOR:
    case AMDGPU::SI_KILL_F32_COND_IMM_TERMINATOR:
    case AMDGPU::SI_DEMOTE_I1:
    case AMDGPU::ENTER_STRICT_WQM:
    case AMDGPU::ENTER_STRICT_WWM:
    case AMDGPU::EXIT_STRICT_WQM:
    case AMDGPU::EXIT_STRICT_WWM:
      Handled = true;
      break;
    default:
      break;
    }
    if (Handled)
      break;
    EntryMI++;
  }
  return EntryMI;
}

void AMDGPUWholeQuadMode::setupLiveMaskReg(MachineBasicBlock *EntryMBB,
                                           MachineBasicBlock::iterator InsPt) {
  LiveMaskReg = MRI->createVirtualRegister(TRI->getBoolRC());
  MachineInstr *CopyExecMI =
      BuildMI(*EntryMBB, InsPt, DebugLoc(), TII->get(AMDGPU::COPY), LiveMaskReg)
          .addReg(LMC.ExecReg);
  LIS->InsertMachineInstrInMaps(*CopyExecMI);
}

bool AMDGPUWholeQuadMode::run(MachineFunction &MF) {
  LLVM_DEBUG(dbgs() << "AMDGPU Whole Quad Mode on " << MF.getName()
                    << " ------------- \n");
  LLVM_DEBUG(MF.dump());

  Blocks.clear();
  DivergentByKill.clear();
  LiveMaskQueries.clear();
  LowerToCopyInstrs.clear();
  KillInstrs.clear();
  WQMExits.clear();

  MachineBasicBlock *Entry = &(MF.front());
  const char GlobalFlags = analyzeFunction(MF);
  if (!(GlobalFlags & StateWQM))
    return lowerNoWQM(Entry);

  if (MF.getFunction().getCallingConv() != CallingConv::AMDGPU_PS) {
    LLVM_DEBUG(
        dbgs() << "WQM found in non-PS shader; result may not be as intended.");
    // FIXME: remove this check?
  }

  // Trace from every WQM requiring block to entry block.
  // This marks all blocks that are required to be in WQM.
  std::queue<MachineBasicBlock *> Worklist;
  for (MachineBasicBlock &StartMBB : MF) {
    BlockInfo &StartBBI = Blocks[&StartMBB];
    if (!(StartBBI.Flags & StateWQM) || StartBBI.ExecMode)
      continue;

    Worklist.push(&StartMBB);
    do {
      MachineBasicBlock *CurrMBB = Worklist.front();
      Worklist.pop();

      // If BlockInfo has ExecMode then this block has been visited already.
      BlockInfo &BBI = Blocks[CurrMBB];
      if (BBI.ExecMode)
        continue;

      LLVM_DEBUG(dbgs() << "WQM Block: " << printMBBReference(*CurrMBB)
                        << "\n");
      BBI.ExecMode = StateWQM;
      for (MachineBasicBlock *PredMBB : CurrMBB->predecessors())
        Worklist.push(PredMBB);
    } while (!Worklist.empty());
  }

  // Mark all blocks impacted by kill divergence.
  for (MachineInstr *KillMI : KillInstrs) {
    auto *StartMBB = KillMI->getParent();
    for (MachineBasicBlock *SuccMBB : StartMBB->successors())
      Worklist.push(SuccMBB);
    while (!Worklist.empty()) {
      MachineBasicBlock *CurrMBB = Worklist.front();
      Worklist.pop();
      if (!DivergentByKill.insert(CurrMBB).second)
        continue;
      for (MachineBasicBlock *SuccMBB : CurrMBB->successors())
        Worklist.push(SuccMBB);
    }
  }

  // Determine WQM exit candidates:
  // 1a. WQM blocks with all non-WQM successors.
  // 1b. WQM return blocks.
  // 2. Non-WQM blocks with any WQM predecessors.
  SmallVector<MachineBasicBlock *> ExitCandidates;
  for (MachineBasicBlock &MBB : MF) {
    BlockInfo &BBI = Blocks[&MBB];
    if (BBI.ExecMode == StateWQM) {
      LLVM_DEBUG(dbgs() << "WQM Block: " << printMBBReference(MBB) << "\n");
      if (llvm::all_of(MBB.successors(), [&](MachineBasicBlock *SuccMBB) {
            return Blocks[SuccMBB].ExecMode != StateWQM;
          })) {
        ExitCandidates.push_back(&MBB);
      } else if (MBB.succ_empty()) {
        for (auto &Term : MBB.terminators()) {
          if (Term.isReturn()) {
            ExitCandidates.push_back(&MBB);
            break;
          }
        }
      }
    } else {
      LLVM_DEBUG(dbgs() << "Non-WQM Block: " << printMBBReference(MBB) << "\n");
      if (llvm::any_of(MBB.predecessors(), [&](MachineBasicBlock *SuccMBB) {
            return Blocks[SuccMBB].ExecMode == StateWQM;
          })) {
        ExitCandidates.push_back(&MBB);
      }
    }
  }

  // Perform exec mask analysis, ignoring instructions lowered in this pass.
  auto DiscardFilter = [&](MachineInstr *MI) {
    switch (MI->getOpcode()) {
    case AMDGPU::SI_KILL_I1_TERMINATOR:
    case AMDGPU::SI_KILL_F32_COND_IMM_TERMINATOR:
    case AMDGPU::SI_DEMOTE_I1:
      return false;
    default:
      return true;
    }
  };
  EMA.analyze(MF, LIS, /*Filter=*/DiscardFilter);

  assert(!ExitCandidates.empty() &&
         "There should be at least one WQM exit candidate.");

  auto EntryVN = EMA.getPrincipleVN(Entry);

  // Try to establish a single uniform exit block.
  SmallSet<MachineBasicBlock *, 4> Visited;
  MachineBasicBlock *UniformExit = nullptr;
  assert(Worklist.empty());
  for (MachineBasicBlock *ExitMBB : ExitCandidates)
    Worklist.push(ExitMBB);
  do {
    MachineBasicBlock *MBB = Worklist.front();
    Worklist.pop();

    if (!Visited.insert(MBB).second)
      continue;

    if (EMA.getPrincipleVN(MBB) != EntryVN) {
      // Not uniform; keep searching
      for (auto *Succ : MBB->successors())
        Worklist.push(Succ);
    } else if (llvm::all_of(ExitCandidates, [&](MachineBasicBlock *ExitMBB) {
                 return PDT->dominates(MBB, ExitMBB);
               })) {
      UniformExit = MBB;
      break;
    }
  } while (!Worklist.empty());

  //
  // MIR modification starts here
  //

  // Store initial EXEC -- after any setup.
  auto EntryMI = findEntryMI(Entry);
  setupLiveMaskReg(Entry, EntryMI);

  // Enter WQM
  MachineInstr *EnterWQMMI =
      BuildMI(*Entry, EntryMI, DebugLoc(), TII->get(LMC.WQMOpc), LMC.ExecReg)
          .addReg(LMC.ExecReg);
  LIS->InsertMachineInstrInMaps(*EnterWQMMI);

  // Apply WQM exits
  if (UniformExit) {
    LLVM_DEBUG(dbgs() << "Uniform WQM Exit = "
                      << printMBBReference(*UniformExit) << "\n");
    insertWQMExit(UniformExit);
  } else {
    // Assume all exit candidates are divergent and exit WQM independently.
    for (auto *ExitMBB : ExitCandidates) {
      LLVM_DEBUG(dbgs() << "Divergent WQM Exit: " << printMBBReference(*ExitMBB)
                        << "\n");
      insertWQMExit(ExitMBB);
    }
  }

  // Perform lowering for mixed mode, live mask queries, discards
  lowerCopyInstrs();
  for (auto &BII : Blocks) {
    auto *MBB = BII.first;
    BlockInfo &Info = BII.second;
    if (Info.Flags &
        (StateExact | FlagLiveMaskQuery | FlagKillInstr | StateStrictWQM)) {
      auto BlockVN = EMA.getPrincipleVN(MBB);
      lowerBlock(*MBB, Info, BlockVN == EntryVN);
    }
  }

  LIS->createAndComputeVirtRegInterval(LiveMaskReg);
  LIS->removeAllRegUnitsForPhysReg(AMDGPU::SCC);
  LIS->removeAllRegUnitsForPhysReg(AMDGPU::EXEC);
  return true;
}

bool AMDGPUWholeQuadModeLegacy::runOnMachineFunction(MachineFunction &MF) {
  LiveIntervals *LIS = &getAnalysis<LiveIntervalsWrapperPass>().getLIS();
  auto *MDTWrapper = getAnalysisIfAvailable<MachineDominatorTreeWrapperPass>();
  MachineDominatorTree *MDT = MDTWrapper ? &MDTWrapper->getDomTree() : nullptr;
  MachinePostDominatorTree *PDT =
      &getAnalysis<MachinePostDominatorTreeWrapperPass>().getPostDomTree();
  AMDGPUWholeQuadMode Impl(MF, LIS, MDT, PDT);
  return Impl.run(MF);
}

PreservedAnalyses
AMDGPUWholeQuadModePass::run(MachineFunction &MF,
                             MachineFunctionAnalysisManager &MFAM) {
  MFPropsModifier _(*this, MF);

  LiveIntervals *LIS = &MFAM.getResult<LiveIntervalsAnalysis>(MF);
  MachineDominatorTree *MDT =
      MFAM.getCachedResult<MachineDominatorTreeAnalysis>(MF);
  MachinePostDominatorTree *PDT =
      &MFAM.getResult<MachinePostDominatorTreeAnalysis>(MF);
  AMDGPUWholeQuadMode Impl(MF, LIS, MDT, PDT);
  bool Changed = Impl.run(MF);
  if (!Changed)
    return PreservedAnalyses::all();

  PreservedAnalyses PA = getMachineFunctionPassPreservedAnalyses();
  PA.preserve<SlotIndexesAnalysis>();
  PA.preserve<LiveIntervalsAnalysis>();
  PA.preserve<MachineDominatorTreeAnalysis>();
  PA.preserve<MachinePostDominatorTreeAnalysis>();
  return PA;
}
