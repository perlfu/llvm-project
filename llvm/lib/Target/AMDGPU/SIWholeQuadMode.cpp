//===-- SIWholeQuadMode.cpp - enter and suspend whole quad mode -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This pass adds instructions to enable whole quad mode (strict or non-strict)
/// for pixel shaders, and strict whole wavefront mode for all programs.
///
/// The "strict" prefix indicates that inactive lanes do not take part in
/// control flow, specifically an inactive lane enabled by a strict WQM/WWM will
/// always be enabled irrespective of control flow decisions. Conversely in
/// non-strict WQM inactive lanes may control flow decisions.
///
/// Whole quad mode is required for derivative computations, but it interferes
/// with shader side effects (stores and atomics). It ensures that WQM is
/// enabled when necessary, but disabled around stores and atomics.
///
/// When necessary, this pass creates a function prolog
///
///   S_MOV_B64 LiveMask, EXEC
///   S_WQM_B64 EXEC, EXEC
///
/// to enter WQM at the top of the function and surrounds blocks of Exact
/// instructions by
///
///   S_AND_SAVEEXEC_B64 Tmp, LiveMask
///   ...
///   S_MOV_B64 EXEC, Tmp
///
/// We also compute when a sequence of instructions requires strict whole
/// wavefront mode (StrictWWM) and insert instructions to save and restore it:
///
///   S_OR_SAVEEXEC_B64 Tmp, -1
///   ...
///   S_MOV_B64 EXEC, Tmp
///
/// When a sequence of instructions requires strict whole quad mode (StrictWQM)
/// we use a similar save and restore mechanism and force whole quad mode for
/// those instructions:
///
///  S_MOV_B64 Tmp, EXEC
///  S_WQM_B64 EXEC, EXEC
///  ...
///  S_MOV_B64 EXEC, Tmp
///
/// In order to avoid excessive switching during sequences of Exact
/// instructions, the pass first analyzes which instructions must be run in WQM
/// (aka which instructions produce values that lead to derivative
/// computations).
///
/// Basic blocks are always exited in WQM as long as some successor needs WQM.
///
/// There is room for improvement given better control flow analysis:
///
///  (1) at the top level (outside of control flow statements, and as long as
///      kill hasn't been used), one SGPR can be saved by recovering WQM from
///      the LiveMask (this is implemented for the entry block).
///
///  (2) when entire regions (e.g. if-else blocks or entire loops) only
///      consist of exact and don't-care instructions, the switch only has to
///      be done at the entry and exit points rather than potentially in each
///      block of the region.
///
//===----------------------------------------------------------------------===//

#include "SIWholeQuadMode.h"
#include "AMDGPU.h"
#include "AMDGPUExecMaskAnalysis.h"
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

#define DEBUG_TYPE "si-wqm"

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

class SIWholeQuadMode {
public:
  SIWholeQuadMode(MachineFunction &MF, LiveIntervals *LIS,
                  MachineDominatorTree *MDT, MachinePostDominatorTree *PDT)
      : ST(&MF.getSubtarget<GCNSubtarget>()), TII(ST->getInstrInfo()),
        TRI(&TII->getRegisterInfo()), MRI(&MF.getRegInfo()), LIS(LIS), MDT(MDT),
        PDT(PDT) {}
  bool run(MachineFunction &MF);

private:
  const GCNSubtarget *ST;
  const SIInstrInfo *TII;
  const SIRegisterInfo *TRI;
  MachineRegisterInfo *MRI;
  LiveIntervals *LIS;
  MachineDominatorTree *MDT;
  MachinePostDominatorTree *PDT;
  AMDGPUExecMaskAnalysis EMA;

  unsigned AndOpc;
  unsigned AndTermOpc;
  unsigned AndN2Opc;
  unsigned AndN2TermOpc;
  unsigned OrOpc;
  unsigned OrSaveExecOpc;
  unsigned XorOpc;
  unsigned XorTermOpc;
  unsigned MovOpc;
  unsigned MovTermOpc;
  unsigned AndSaveExecOpc;
  unsigned AndSaveExecTermOpc;
  unsigned WQMOpc;
  Register ExecReg;
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

  void splitBlock(MachineInstr *TermMI);
  MachineInstr *lowerKillI1(MachineInstr &MI, bool IsWQM);
  MachineInstr *lowerKillF32(MachineInstr &MI);

  void lowerBlock(MachineBasicBlock &MBB, BlockInfo &BI, bool HasEntryExec);

  void lowerCopyInstrs();
  bool lowerKillInstrs(bool IsWQM);
  bool lowerNoWQM();
  void lowerLiveMaskQuery(MachineInstr *MI, Register Src);
};

class SIWholeQuadModeLegacy : public MachineFunctionPass {
public:
  static char ID;

  SIWholeQuadModeLegacy() : MachineFunctionPass(ID) {}

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override { return "SI Whole Quad Mode"; }

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
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::IsSSA);
  }
};
} // end anonymous namespace

char SIWholeQuadModeLegacy::ID = 0;

INITIALIZE_PASS_BEGIN(SIWholeQuadModeLegacy, DEBUG_TYPE, "SI Whole Quad Mode",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(LiveIntervalsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachinePostDominatorTreeWrapperPass)
INITIALIZE_PASS_END(SIWholeQuadModeLegacy, DEBUG_TYPE, "SI Whole Quad Mode",
                    false, false)

char &llvm::SIWholeQuadModeID = SIWholeQuadModeLegacy::ID;

FunctionPass *llvm::createSIWholeQuadModeLegacyPass() {
  return new SIWholeQuadModeLegacy;
}

#ifndef NDEBUG
LLVM_DUMP_METHOD void SIWholeQuadMode::printInfo() {
  // FIXME: make this work again
  for (const auto &BII : Blocks) {
    dbgs() << "\n"
           << printMBBReference(*BII.first) << ":\n"
           << ", Flags = " << PrintState(BII.second.Flags)
           << ", ExecMode = " << BII.second.ExecMode << "\n\n";
  }
}
#endif

bool SIWholeQuadMode::needsExactMode(const MachineInstr &MI) {
  // Dual source blend export acts as implicit strict-wqm, its sources
  // need to be shuffled in strict wqm, but the export itself needs to
  // run in exact mode.
  return TII->isDisableWQM(MI) || TII->isDualSourceBlendEXP(MI);
}

// Scan instructions to determine which ones require an Exact execmask and
// which ones seed WQM requirements.
char SIWholeQuadMode::analyzeFunction(MachineFunction &MF) {
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
          Flags = StateWQM;
          dbgs() << "WQM: " << MI;
        }
      } else if (Opcode == AMDGPU::WQM) {
        // The WQM intrinsic requires its output to have all the helper lanes
        // correct, so we need it to be in WQM.
        Flags = StateWQM;
        LowerToCopyInstrs.insert(&MI);
        dbgs() << "WQM: " << MI;
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
SIWholeQuadMode::saveSCC(MachineBasicBlock &MBB,
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

void SIWholeQuadMode::splitBlock(MachineInstr *TermMI) {
  MachineBasicBlock *BB = TermMI->getParent();
  LLVM_DEBUG(dbgs() << "Split block " << printMBBReference(*BB) << " @ "
                    << *TermMI << "\n");

  MachineBasicBlock *SplitBB =
      BB->splitAt(*TermMI, /*UpdateLiveIns*/ true, LIS);

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
  default:
    break;
  }
  if (NewOpcode)
    TermMI->setDesc(TII->get(NewOpcode));

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

    // Link blocks
    MachineInstr *MI =
        BuildMI(*BB, BB->end(), DebugLoc(), TII->get(AMDGPU::S_BRANCH))
            .addMBB(SplitBB);
    LIS->InsertMachineInstrInMaps(*MI);
  }
}

MachineInstr *SIWholeQuadMode::lowerKillF32(MachineInstr &MI) {
  assert(LiveMaskReg.isVirtual());

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
  Register VCC = ST->isWave32() ? AMDGPU::VCC_LO : AMDGPU::VCC;

  if (TRI->isVGPR(*MRI, Op0.getReg())) {
    Opcode = AMDGPU::getVOPe32(Opcode);
    VcmpMI = BuildMI(MBB, &MI, DL, TII->get(Opcode)).add(Op1).add(Op0);
  } else {
    VcmpMI = BuildMI(MBB, &MI, DL, TII->get(Opcode))
                 .addReg(VCC, RegState::Define)
                 .addImm(0) // src0 modifiers
                 .add(Op1)
                 .addImm(0) // src1 modifiers
                 .add(Op0)
                 .addImm(0); // omod
  }

  MachineInstr *MaskUpdateMI =
      BuildMI(MBB, MI, DL, TII->get(AndN2Opc), LiveMaskReg)
          .addReg(LiveMaskReg)
          .addReg(VCC);

  // State of SCC represents whether any lanes are live in mask,
  // if SCC is 0 then no lanes will be alive anymore.
  MachineInstr *EarlyTermMI =
      BuildMI(MBB, MI, DL, TII->get(AMDGPU::SI_EARLY_TERMINATE_SCC0));

  MachineInstr *ExecMaskMI = BuildMI(MBB, MI, DL, TII->get(AndN2Opc), ExecReg)
                                 .addReg(ExecReg)
                                 .addReg(VCC);

  assert(MBB.succ_size() == 1);
  MachineInstr *NewTerm = BuildMI(MBB, MI, DL, TII->get(AMDGPU::S_BRANCH))
                              .addMBB(*MBB.succ_begin());

  // Update live intervals
  LIS->ReplaceMachineInstrInMaps(MI, *VcmpMI);
  MBB.remove(&MI);

  LIS->InsertMachineInstrInMaps(*MaskUpdateMI);
  LIS->InsertMachineInstrInMaps(*ExecMaskMI);
  LIS->InsertMachineInstrInMaps(*EarlyTermMI);
  LIS->InsertMachineInstrInMaps(*NewTerm);

  return NewTerm;
}

MachineInstr *SIWholeQuadMode::lowerKillI1(MachineInstr &MI, bool IsWQM) {
  assert(LiveMaskReg.isVirtual());

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
      MaskUpdateMI = BuildMI(MBB, MI, DL, TII->get(AndN2Opc), LiveMaskReg)
                         .addReg(LiveMaskReg)
                         .addReg(ExecReg);
    } else {
      // Static: kill does nothing
      MachineInstr *NewTerm = nullptr;
      if (MI.getOpcode() == AMDGPU::SI_DEMOTE_I1) {
        LIS->RemoveMachineInstrFromMaps(MI);
      } else {
        assert(MBB.succ_size() == 1);
        NewTerm = BuildMI(MBB, MI, DL, TII->get(AMDGPU::S_BRANCH))
                      .addMBB(*MBB.succ_begin());
        LIS->ReplaceMachineInstrInMaps(MI, *NewTerm);
      }
      MBB.remove(&MI);
      return NewTerm;
    }
  } else {
    if (!KillVal) {
      // Op represents live lanes after kill,
      // so exec mask needs to be factored in.
      TmpReg = MRI->createVirtualRegister(TRI->getBoolRC());
      ComputeKilledMaskMI = BuildMI(MBB, MI, DL, TII->get(AndN2Opc), TmpReg)
                                .addReg(ExecReg)
                                .add(Op);
      MaskUpdateMI = BuildMI(MBB, MI, DL, TII->get(AndN2Opc), LiveMaskReg)
                         .addReg(LiveMaskReg)
                         .addReg(TmpReg);
    } else {
      // Op represents lanes to kill
      MaskUpdateMI = BuildMI(MBB, MI, DL, TII->get(AndN2Opc), LiveMaskReg)
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
  MachineInstr *NewTerm;
  MachineInstr *WQMMaskMI = nullptr;
  Register LiveMaskWQM;
  if (IsDemote) {
    // Demote - deactivate quads with only helper lanes
    LiveMaskWQM = MRI->createVirtualRegister(TRI->getBoolRC());
    WQMMaskMI =
        BuildMI(MBB, MI, DL, TII->get(WQMOpc), LiveMaskWQM).addReg(LiveMaskReg);
    NewTerm = BuildMI(MBB, MI, DL, TII->get(AndOpc), ExecReg)
                  .addReg(ExecReg)
                  .addReg(LiveMaskWQM);
  } else {
    // Kill - deactivate lanes no longer in live mask
    if (Op.isImm()) {
      NewTerm = BuildMI(MBB, &MI, DL, TII->get(MovOpc), ExecReg).addImm(0);
    } else if (!IsWQM) {
      NewTerm = BuildMI(MBB, &MI, DL, TII->get(AndOpc), ExecReg)
                    .addReg(ExecReg)
                    .addReg(LiveMaskReg);
    } else {
      unsigned Opcode = KillVal ? AndN2Opc : AndOpc;
      NewTerm = BuildMI(MBB, &MI, DL, TII->get(Opcode), ExecReg)
                    .addReg(ExecReg)
                    .add(Op);
    }
  }

  // Update live intervals
  LIS->RemoveMachineInstrFromMaps(MI);
  MBB.remove(&MI);
  assert(EarlyTermMI);
  assert(MaskUpdateMI);
  assert(NewTerm);
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
void SIWholeQuadMode::lowerBlock(MachineBasicBlock &MBB, BlockInfo &BI,
                                 bool HasEntryExec) {
  LLVM_DEBUG(dbgs() << "\nLowering block " << printMBBReference(MBB) << ":\n");

  const bool DivergencesByKill = DivergentByKill.contains(&MBB);
  SmallVector<MachineInstr *, 4> SplitPoints;
  char BlockMode = BI.ExecMode == StateWQM ? StateWQM : StateExact;
  char State = BlockMode;
  Register SavedWQMReg = Register();

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
              .addReg(ExecReg);
      MachineInstr *ExitMI =
          BuildMI(MBB, I, DebugLoc(), TII->get(AndOpc), ExecReg)
              .addReg(ExecReg)
              .addReg(LiveMaskReg);
      LIS->InsertMachineInstrInMaps(*SaveExecMI);
      LIS->InsertMachineInstrInMaps(*ExitMI);
      State = StateExact;

      continue;
    } else if (BlockMode == StateWQM && State == StateExact) {
      // Restore WQM
      assert(SavedWQMReg);
      MachineInstr *RestoreMI = BuildMI(MBB, MI.getIterator(), DebugLoc(),
                                        TII->get(AMDGPU::COPY), ExecReg)
                                    .addReg(SavedWQMReg);
      LIS->InsertMachineInstrInMaps(*RestoreMI);
      LIS->createAndComputeVirtRegInterval(SavedWQMReg);
      SavedWQMReg = Register();
      State = StateWQM;
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
      lowerLiveMaskQuery(&MI, State == StateWQM ? LiveMaskReg : ExecReg);
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
      if (HasEntryExec && BlockMode == StateWQM && !DivergencesByKill) {
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
        BuildMI(MBB, MBB.end(), DebugLoc(), TII->get(AMDGPU::COPY), ExecReg)
            .addReg(SavedWQMReg);
    LIS->InsertMachineInstrInMaps(*RestoreMI);
    LIS->createAndComputeVirtRegInterval(SavedWQMReg);
    State = StateWQM;
  }

  // Perform splitting after instruction scan to simplify iteration.
  for (MachineInstr *MI : SplitPoints)
    splitBlock(MI);
}

void SIWholeQuadMode::lowerLiveMaskQuery(MachineInstr *MI, Register Src) {
  const DebugLoc &DL = MI->getDebugLoc();
  Register Dest = MI->getOperand(0).getReg();

  MachineInstr *Copy =
      BuildMI(*MI->getParent(), MI, DL, TII->get(AMDGPU::COPY), Dest)
          .addReg(Src);

  LIS->ReplaceMachineInstrInMaps(*MI, *Copy);
  MI->eraseFromParent();
}

void SIWholeQuadMode::lowerCopyInstrs() {
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

bool SIWholeQuadMode::isSCCLiveAt(const MachineInstr *MI) {
  LiveRange &LR =
      LIS->getRegUnit(*TRI->regunits(MCRegister::from(AMDGPU::SCC)).begin());
  return LR.liveAt(LIS->getInstructionIndex(*MI));
}

void SIWholeQuadMode::insertWQMExit(MachineBasicBlock *MBB) {
  BlockInfo &BBI = Blocks[MBB];
  MachineBasicBlock::iterator I;
  if (!(BBI.Flags & StateWQM)) {
    // Block does not expect WQM; exit at start.
    // Note: SCC should not be live at block start
    I = MBB->instr_begin() == MBB->instr_end()
            ? MBB->SkipPHIsLabelsAndDebug(MBB->instr_begin())
            : MBB->instr_end();
    return;
  } else {
    assert(BBI.LastWQMUser);
    I = std::next(BBI.LastWQMUser->getIterator());
    if (isSCCLiveAt(&*I))
      I = saveSCC(*MBB, I);
  }

  MachineInstr *MI = BuildMI(*MBB, I, DebugLoc(), TII->get(AndOpc), ExecReg)
                         .addReg(ExecReg)
                         .addReg(LiveMaskReg);
  LIS->InsertMachineInstrInMaps(*MI);
  WQMExits.insert(MI);
}

bool SIWholeQuadMode::lowerNoWQM() {
  LiveMaskReg = ExecReg;
  lowerCopyInstrs();
  for (MachineInstr *MI : LiveMaskQueries)
    lowerLiveMaskQuery(MI, ExecReg);
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
  return LowerToCopyInstrs.size() || LiveMaskQueries.size() ||
         KillInstrs.size();
}

bool SIWholeQuadMode::run(MachineFunction &MF) {
  // WQM only applies to pixel shaders
  // FIXME: but need to lower WQM to copies in other contexts?
  if (MF.getFunction().getCallingConv() != CallingConv::AMDGPU_PS)
    return false;

  LLVM_DEBUG(dbgs() << "SI Whole Quad Mode on " << MF.getName()
                    << " ------------- \n");
  LLVM_DEBUG(MF.dump(););

  Blocks.clear();
  DivergentByKill.clear();
  LiveMaskQueries.clear();
  LowerToCopyInstrs.clear();
  KillInstrs.clear();
  WQMExits.clear();

  if (ST->isWave32()) {
    AndOpc = AMDGPU::S_AND_B32;
    AndTermOpc = AMDGPU::S_AND_B32_term;
    AndN2Opc = AMDGPU::S_ANDN2_B32;
    AndN2TermOpc = AMDGPU::S_ANDN2_B32_term;
    OrOpc = AMDGPU::S_OR_B32;
    OrSaveExecOpc = AMDGPU::S_OR_SAVEEXEC_B32;
    XorOpc = AMDGPU::S_XOR_B32;
    XorTermOpc = AMDGPU::S_XOR_B32_term;
    MovOpc = AMDGPU::S_MOV_B32;
    MovTermOpc = AMDGPU::S_MOV_B32_term;
    AndSaveExecOpc = AMDGPU::S_AND_SAVEEXEC_B32;
    AndSaveExecTermOpc = AMDGPU::S_AND_SAVEEXEC_B32_term;
    WQMOpc = AMDGPU::S_WQM_B32;
    ExecReg = AMDGPU::EXEC_LO;
  } else {
    AndOpc = AMDGPU::S_AND_B64;
    AndTermOpc = AMDGPU::S_AND_B64_term;
    AndN2Opc = AMDGPU::S_ANDN2_B64;
    AndN2TermOpc = AMDGPU::S_ANDN2_B64_term;
    OrOpc = AMDGPU::S_OR_B64;
    OrSaveExecOpc = AMDGPU::S_OR_SAVEEXEC_B64;
    XorOpc = AMDGPU::S_XOR_B64;
    XorTermOpc = AMDGPU::S_XOR_B64_term;
    MovOpc = AMDGPU::S_MOV_B64;
    MovTermOpc = AMDGPU::S_MOV_B64_term;
    AndSaveExecOpc = AMDGPU::S_AND_SAVEEXEC_B64;
    AndSaveExecTermOpc = AMDGPU::S_AND_SAVEEXEC_B64_term;
    WQMOpc = AMDGPU::S_WQM_B64;
    ExecReg = AMDGPU::EXEC;
  }

  const char GlobalFlags = analyzeFunction(MF);
  if (!(GlobalFlags & StateWQM))
    return lowerNoWQM();

  // Trace from every WQM requiring block to entry block.
  // This marks all blocks that are required to be in WQM.
  std::queue<MachineBasicBlock *> Worklist;
  MachineBasicBlock *Entry = &(MF.front());
  for (MachineBasicBlock &StartMBB : MF) {
    BlockInfo &StartBBI = Blocks[&StartMBB];
    dbgs() << "MBB: " << printMBBReference(StartMBB) << " "
           << ((int)StartBBI.Flags) << " " << ((int)StartBBI.ExecMode) << "\n";
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

      dbgs() << "Mark: " << printMBBReference(*CurrMBB) << "\n";
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
    do {
      MachineBasicBlock *CurrMBB = Worklist.front();
      Worklist.pop();
      if (!DivergentByKill.insert(CurrMBB).second)
        continue;
      for (MachineBasicBlock *SuccMBB : CurrMBB->successors())
        Worklist.push(SuccMBB);
    } while (!Worklist.empty());
  }

  // Determine WQM blocks with non-WQM successors.
  // These are WQM exit candidates.
  SmallVector<MachineBasicBlock *> ExitCandidates;
  for (MachineBasicBlock &MBB : MF) {
    BlockInfo &BBI = Blocks[&MBB];
    if (!(BBI.Flags & StateWQM))
      continue;
    dbgs() << "WQM Block: " << printMBBReference(MBB) << "\n";
    if (llvm::any_of(MBB.successors(), [&](MachineBasicBlock *SuccMBB) {
          return !Blocks[SuccMBB].ExecMode;
        })) {
      ExitCandidates.push_back(&MBB);
    } else if (MBB.succ_empty()) {
      // Consider return blocks in WQM as exit candidates.
      for (auto &Term : MBB.terminators()) {
        if (Term.isReturn()) {
          ExitCandidates.push_back(&MBB);
          break;
        }
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
  LiveMaskReg = MRI->createVirtualRegister(TRI->getBoolRC());
  MachineBasicBlock::iterator EntryMI = Entry->getFirstNonPHI();
  while (EntryMI->modifiesRegister(ExecReg, TRI)) {
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
  MachineInstr *SaveExecMI =
      BuildMI(*Entry, EntryMI, DebugLoc(), TII->get(AMDGPU::COPY), LiveMaskReg)
          .addReg(ExecReg);
  LIS->InsertMachineInstrInMaps(*SaveExecMI);

  // Enter WQM
  MachineInstr *EnterWQMMI =
      BuildMI(*Entry, EntryMI, DebugLoc(), TII->get(WQMOpc), ExecReg)
          .addReg(ExecReg);
  LIS->InsertMachineInstrInMaps(*EnterWQMMI);

  // Apply WQM exits
  if (UniformExit) {
    dbgs() << "UniformExit = " << printMBBReference(*UniformExit) << "\n";
    insertWQMExit(UniformExit);
  } else {
    // Assume all exit candidates are divergent and exit WQM independently.
    for (auto *ExitMBB : ExitCandidates)
      insertWQMExit(ExitMBB);
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

bool SIWholeQuadModeLegacy::runOnMachineFunction(MachineFunction &MF) {
  LiveIntervals *LIS = &getAnalysis<LiveIntervalsWrapperPass>().getLIS();
  auto *MDTWrapper = getAnalysisIfAvailable<MachineDominatorTreeWrapperPass>();
  MachineDominatorTree *MDT = MDTWrapper ? &MDTWrapper->getDomTree() : nullptr;
  MachinePostDominatorTree *PDT =
      &getAnalysis<MachinePostDominatorTreeWrapperPass>().getPostDomTree();
  SIWholeQuadMode Impl(MF, LIS, MDT, PDT);
  return Impl.run(MF);
}

PreservedAnalyses
SIWholeQuadModePass::run(MachineFunction &MF,
                         MachineFunctionAnalysisManager &MFAM) {
  MFPropsModifier _(*this, MF);

  LiveIntervals *LIS = &MFAM.getResult<LiveIntervalsAnalysis>(MF);
  MachineDominatorTree *MDT =
      MFAM.getCachedResult<MachineDominatorTreeAnalysis>(MF);
  MachinePostDominatorTree *PDT =
      MFAM.getCachedResult<MachinePostDominatorTreeAnalysis>(MF);
  SIWholeQuadMode Impl(MF, LIS, MDT, PDT);
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
