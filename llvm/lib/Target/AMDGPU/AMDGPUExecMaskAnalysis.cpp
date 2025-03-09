//===-- AMDGPUExecMaskAnalysis.cpp - analysis of exec register --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// \brief Enumerate exec register values within function and infer
/// relationships between these values.
///
//===----------------------------------------------------------------------===//

#include "AMDGPUExecMaskAnalysis.h"
#include "AMDGPU.h"
#include "AMDGPULaneMaskUtils.h"
#include "GCNSubtarget.h"
#include "MCTargetDesc/AMDGPUMCTargetDesc.h"
#include "llvm/Support/raw_ostream.h"
#include <queue>

using namespace llvm;

#define DEBUG_TYPE "amdgpu-exec-mask-analysis"

#ifndef NDEBUG
LLVM_DUMP_METHOD void AMDGPUExecMaskAnalysis::printInfo() {
  for (auto &MBB : *MF) {
    dbgs() << "Block: " << printMBBReference(MBB) << "\n";
    for (MachineInstr &MI : MBB) {
      unsigned VN = VNs.findLeader(InstrVN[&MI]);
      dbgs() << "VN: " << VN << " for " << MI;
    }
  }
}
#endif

bool AMDGPUExecMaskAnalysis::isZeroVN(unsigned VN) {
  return VNs.findLeader(VN) == VNs.findLeader(ZeroVN);
}

std::optional<unsigned>
AMDGPUExecMaskAnalysis::getEdgeVN(MachineBasicBlock *Src,
                                  MachineBasicBlock *Dst) {
  // Cannot provide values if Src has not been mapped
  if (!BlockVN.contains(Src))
    return std::nullopt;

  // Entry VN for this block is last VN for predecessor
  if (Src->size() == 0)
    return VNs.findLeader(BlockVN[Src]);

  for (auto I = Src->instr_rbegin(), E = Src->instr_rend(); I != E; ++I) {
    switch (I->getOpcode()) {
    case AMDGPU::S_BRANCH:
    case AMDGPU::S_CBRANCH_EXECZ:
    case AMDGPU::S_CBRANCH_EXECNZ:
    case AMDGPU::S_CBRANCH_VCCZ:
    case AMDGPU::S_CBRANCH_VCCNZ:
      if (I->getOperand(0).isMBB() && I->getOperand(0).getMBB() == Dst) {
        if (I->getOpcode() == AMDGPU::S_CBRANCH_EXECZ)
          return VNs.findLeader(ZeroVN);
        // Note: instructions after EXECNZ will have ZeroVN set already.
        return VNs.findLeader(InstrVN[&*I]);
      }
      break;
    default:
      break;
    }
  }
  // Assume fall-through
  MachineInstr *LastMI = &Src->instr_back();
  if (VNDef.contains(LastMI))
    return VNs.findLeader(VNDef[LastMI]);
  // Instruction after S_CBRANCH_EXECNZ is at EXEC=0.
  if (LastMI->getOpcode() == AMDGPU::S_CBRANCH_EXECNZ)
    return VNs.findLeader(ZeroVN);
  return VNs.findLeader(InstrVN[LastMI]);
}

bool AMDGPUExecMaskAnalysis::hasExecUse(MachineInstr *MI) {
  for (auto &Op : MI->explicit_operands()) {
    if (!Op.isUse())
      continue;
    if (Op.isReg() && Op.getReg() == LMC->ExecReg)
      return true;
  }
  return false;
}

MachineOperand *AMDGPUExecMaskAnalysis::getNonExecUse(MachineInstr *MI) {
  for (auto &Op : MI->explicit_operands()) {
    if (!Op.isUse())
      continue;
    if (Op.isReg() && Op.getReg() == LMC->ExecReg)
      continue;
    return &Op;
  }
  return nullptr;
}

void AMDGPUExecMaskAnalysis::clear() {
  InstrVN.clear();
  BlockVN.clear();
  VNDef.clear();
  VNs.clear();
  VNs.grow(ZeroVN + 1);
}

void AMDGPUExecMaskAnalysis::analyze(MachineFunction &TheMF, LiveIntervals *LIS,
                                     FilterFn Filter) {
  clear();

  MF = &TheMF;
  const GCNSubtarget *ST = &MF->getSubtarget<GCNSubtarget>();
  const SIInstrInfo *TII = ST->getInstrInfo();
  const SIRegisterInfo *TRI = &TII->getRegisterInfo();
  MachineRegisterInfo *MRI = &MF->getRegInfo();
  LMC = &AMDGPU::LaneMaskConstants::get(*ST);

  DenseMap<unsigned, MachineInstr *> VNDefRev;
  DenseMap<unsigned, MachineBasicBlock *> PhiVNs;
  unsigned NextVN = ZeroVN + 1;

  std::queue<MachineBasicBlock *> Worklist;
  MachineBasicBlock *Entry = &(MF->front());
  Worklist.push(Entry);
  do {
    MachineBasicBlock *MBB = Worklist.front();
    Worklist.pop();

    // Already visited this block?
    if (BlockVN.contains(MBB))
      continue;

    // Compute block entry VN
    if (MBB->pred_size() == 0) {
      VNs.grow(NextVN + 1);
      BlockVN[MBB] = NextVN++;
    } else if (auto *Pred = MBB->getSinglePredecessor()) {
      auto PredVN = getEdgeVN(Pred, MBB);
      if (!PredVN.has_value()) {
        // Wait until predescessor has value
        Worklist.push(MBB);
        continue;
      }
      BlockVN[MBB] = *PredVN;
    } else {
      std::optional<unsigned> PredVN = std::nullopt;
      bool Congruent = true;
      bool MissingVNs = false;
      for (auto *Pred : MBB->predecessors()) {
        auto ThisPredVN = getEdgeVN(Pred, MBB);
        MissingVNs = MissingVNs || !ThisPredVN.has_value();
        if (!PredVN.has_value()) {
          PredVN = ThisPredVN;
        } else if (ThisPredVN.has_value() && isZeroVN(*ThisPredVN)) {
          // Ignore EXECZ edges
        } else if (!ThisPredVN.has_value() || *PredVN != *ThisPredVN) {
          // Always add a PHI when only some predecessor edges are available.
          // This guarantees forward progress in cycles.
          // Unnecessary PHIs can be removed later.
          Congruent = false;
          break;
        }
      }
      if (!PredVN.has_value() && MissingVNs) {
        // Wait until at least one predescessor value exists
        Worklist.push(MBB);
        continue;
      }
      if (Congruent) {
        // All predecessors have same value
        if (PredVN.has_value()) {
          BlockVN[MBB] = *PredVN;
        } else {
          // Edge case: block is only reached via EXECZ edges
          assert(!MissingVNs);
          BlockVN[MBB] = ZeroVN;
        }
      } else {
        // Predecessors have different or unresolved values.
        // In the case of unresolved values we may remove the phi later.
        VNs.grow(NextVN + 1);
        BlockVN[MBB] = NextVN++;
        PhiVNs[BlockVN[MBB]] = MBB;
      }
    }

    // Map VN for all instructions in block.
    // Try to resolve in place simple transitions, e.g. Strict WQM/WWM.
    unsigned CurrentVN = BlockVN[MBB];
    assert(VNs.findLeader(CurrentVN) == CurrentVN);
    unsigned StrictSavedVN = ZeroVN;
    LLVM_DEBUG(dbgs() << "Scan: " << printMBBReference(*MBB) << "\n");
    LLVM_DEBUG(dbgs() << "CurrentVN = " << CurrentVN << " "
                      << (PhiVNs.contains(CurrentVN) ? "PHI" : "") << "\n");
    for (MachineInstr &MI : *MBB) {
      auto Opcode = MI.getOpcode();
      InstrVN[&MI] = CurrentVN;
      if (Opcode == AMDGPU::S_CBRANCH_EXECNZ) {
        // EXEC on next instruction is 0.
        CurrentVN = ZeroVN;
      } else if (MI.modifiesRegister(LMC->ExecReg, TRI) && Filter(&MI)) {
        bool NeedVN = true;
        if (Opcode == AMDGPU::ENTER_STRICT_WQM ||
            Opcode == AMDGPU::ENTER_STRICT_WWM) {
          // Give strict modes VN, although all WWM regions are actually equal.
          StrictSavedVN = CurrentVN;
        } else if (Opcode == AMDGPU::EXIT_STRICT_WQM ||
                   Opcode == AMDGPU::EXIT_STRICT_WQM) {
          // Restore previous VN at strict mode exit.
          assert(StrictSavedVN != ZeroVN);
          CurrentVN = StrictSavedVN;
          NeedVN = false;
        } else if (Opcode == AMDGPU::COPY &&
                   (!MI.getOperand(0).isReg() ||
                    MI.getOperand(0).getReg() != LMC->ExecReg)) {
          // Ignore COPY operations which implicitly define EXEC
          NeedVN = false;
        } else if (Opcode == LMC->OrOpc || Opcode == LMC->OrSaveExecOpc) {
          // Check if this is explicitly restoring a previously saved exec mask
          // and use that VN
          auto *SrcOp = getNonExecUse(&MI);
          if (SrcOp->isReg() && hasExecUse(&MI)) {
            auto *Src = TRI->findReachingDef(SrcOp->getReg(),
                                             SrcOp->getSubReg(), MI, *MRI, LIS);
            if (Src && Src->getOpcode() == AMDGPU::COPY) {
              if (Src->getOperand(1).isReg() &&
                  Src->getOperand(1).getReg() == LMC->ExecReg) {
                CurrentVN = VNs.findLeader(InstrVN[Src]);
                NeedVN = false;
              }
            }
          }
        }
        if (NeedVN) {
          CurrentVN = NextVN++;
          VNs.grow(CurrentVN + 1);
          VNDef[&MI] = CurrentVN;
          VNDefRev[CurrentVN] = &MI;
          LLVM_DEBUG(dbgs() << "New VN: " << CurrentVN << ", " << MI);
        }
      }
    }

    // Enqueue all unvisited successors
    for (MachineBasicBlock *SuccMBB : MBB->successors()) {
      if (!BlockVN.contains(SuccMBB))
        Worklist.push(SuccMBB);
    }
  } while (!Worklist.empty());

  // Collapse redundant Phis
  SmallVector<unsigned> ToRemove;
  do {
    LLVM_DEBUG(dbgs() << "Collapse Phis\n");
    for (unsigned VN : ToRemove)
      PhiVNs.erase(VN);
    ToRemove.clear();
    for (auto &E : PhiVNs) {
      MachineBasicBlock *MBB = E.second;
      unsigned VN = E.first;
      LLVM_DEBUG(dbgs() << "Analyze " << VN << " (" << VNs.findLeader(VN)
                        << ") for " << printMBBReference(*MBB) << "\n");

      std::optional<unsigned> PredVN = std::nullopt;
      bool Congruent = true;
      for (auto *Pred : MBB->predecessors()) {
        auto ThisPredVN = getEdgeVN(Pred, MBB);
        assert(ThisPredVN.has_value());
        LLVM_DEBUG(dbgs() << "  PredVN = " << *ThisPredVN << " for "
                          << printMBBReference(*Pred) << "\n");
        if (isZeroVN(*ThisPredVN)) {
          // Ignore EXECZ edges
        } else if (Pred == MBB && *ThisPredVN == VNs.findLeader(VN)) {
          // Ignore EXEC-invariant self edges
        } else if (PredVN.has_value() && *PredVN != *ThisPredVN) {
          Congruent = false;
          break;
        } else {
          PredVN = ThisPredVN;
        }
      }
      if (Congruent) {
        unsigned NewVN = PredVN.has_value() ? *PredVN : ZeroVN;
        VNs.join(VN, NewVN);
        ToRemove.push_back(VN);
        LLVM_DEBUG(dbgs() << "Join " << VN << " " << NewVN << "\n");
      }
    }
  } while (!ToRemove.empty());

  // Look at blocks starting in EXECZ
  // Note: this is a heuristic based on common CFG structure patterns
  for (auto &E : BlockVN) {
    unsigned VN = VNs.findLeader(E.second);
    if (!isZeroVN(VN))
      continue;
    MachineBasicBlock *MBB = E.first;
    if (MBB->size() == 0)
      continue;
    LLVM_DEBUG(dbgs() << "EXECZ block: " << printMBBReference(*MBB) << "\n");
    MachineInstr *MI = &MBB->front();
    MachineOperand *SrcOp;
    if (MI->getOpcode() == LMC->OrOpc && (SrcOp = getNonExecUse(MI)) &&
        SrcOp->isReg()) {
      // Confirm that this operand represents an accumulated mask of lanes
      // exiting a loop Life span of the register should be: S_MOV 0 init, S_OR
      // update, S_ANDN2 use
      Register SrcReg = SrcOp->getReg();
      LiveInterval &LI = LIS->getInterval(SrcReg);
      MachineInstr *InitMI = nullptr, *UpdateMI = nullptr;
      LiveInterval::Segment *UpdateSeg = nullptr;
      // Find relevant instructions from LiveInterval
      for (auto &S : LI.segments) {
        if (!InitMI)
          InitMI = LIS->getInstructionFromIndex(S.start);
        else if (!UpdateMI) {
          UpdateMI = LIS->getInstructionFromIndex(S.start);
          UpdateSeg = &S;
        }
      }
      bool HasMov0Init = InitMI && InitMI->getOpcode() == LMC->MovOpc &&
                         InitMI->getOperand(1).isImm() &&
                         InitMI->getOperand(1).getImm() == 0;
      bool HasOrUpdate = UpdateMI && UpdateMI->getOpcode() == LMC->OrOpc &&
                         UpdateMI->getOperand(2).isReg() &&
                         UpdateMI->getOperand(2).getReg() == SrcReg;
      bool HasAndN2Use = false;
      if (UpdateSeg) {
        for (auto &UseMI : MRI->use_nodbg_instructions(SrcReg)) {
          SlotIndex Idx = LIS->getInstructionIndex(UseMI);
          if (Idx > UpdateSeg->start && Idx < UpdateSeg->end) {
            if (UseMI.getOpcode() == LMC->AndN2TermOpc &&
                UseMI.readsRegister(LMC->ExecReg, TRI)) {
              HasAndN2Use = true;
              break;
            }
          }
        }
      }
      if (HasMov0Init && HasOrUpdate && HasAndN2Use) {
        unsigned RestoreVN = VNs.findLeader(InstrVN[InitMI]);
        unsigned OldVN = VNs.findLeader(VNDef[MI]);
        LLVM_DEBUG(dbgs() << "Join " << OldVN << " " << RestoreVN << "\n");
        VNs.join(OldVN, RestoreVN);
      }
    }
  }

  // Create data structures to represent the computation used to derive each
  // exec mask. Try to deduce for each computation a single operation, e.g. and,
  // or, xor. Such that this operation is applied to existing mask and an
  // external condition, or two existing masks.

  enum ExecDefType { ED_UNKNOWN = 0, ED_EXT, ED_EXEC, ED_AND, ED_OR, ED_XOR };
  struct ExecDef {
    ExecDefType DefType;
    // Value number of LHS
    unsigned LVN;
    // Value number of RHS
    unsigned RVN;
    // RHS is inverted?
    bool InvertRHS;
    // External condition if RHS is external condition
    MachineInstr *Ext;

    ExecDef()
        : DefType(ED_UNKNOWN), LVN(0), RVN(0), InvertRHS(false), Ext(nullptr) {}
    ExecDef(ExecDefType NT)
        : DefType(NT), LVN(0), RVN(0), InvertRHS(false), Ext(nullptr) {}
    ExecDef(ExecDefType NT, MachineInstr *Src)
        : DefType(NT), LVN(0), RVN(0), InvertRHS(false), Ext(Src) {}
    ExecDef(ExecDefType NT, unsigned L)
        : DefType(NT), LVN(L), RVN(0), InvertRHS(false), Ext(nullptr) {}
    ExecDef(ExecDefType NT, unsigned L, unsigned R, bool Invert = false,
            MachineInstr *Src = nullptr)
        : DefType(NT), LVN(L), RVN(R), InvertRHS(Invert), Ext(Src) {}

#ifndef NDEBUG
    LLVM_DUMP_METHOD void print() const {
      switch (DefType) {
      case ED_UNKNOWN:
        dbgs() << "UNKNOWN";
        break;
      case ED_EXT:
        dbgs() << "EXT";
        break;
      case ED_EXEC:
        dbgs() << "EXEC";
        break;
      case ED_AND:
        dbgs() << "AND";
        break;
      case ED_OR:
        dbgs() << "OR";
        break;
      case ED_XOR:
        dbgs() << "XOR";
        break;
      }
      dbgs() << " LVN: " << LVN << ", RVN: " << (InvertRHS ? "!" : "") << ""
             << RVN;
      if (Ext)
        dbgs() << ", Ext:" << *Ext;
      else
        dbgs() << "\n";
    }
#endif
  };

  DenseMap<unsigned, ExecDef> DefComp;

  auto simplifyExecComp = [&](ExecDefType DT, const ExecDef &LHS,
                              const ExecDef &RHS,
                              bool &Incomplete) -> std::optional<ExecDef> {
    if (DT == ED_OR) {
      // Fold OR (AND X Y) (AND X !Y) -> X
      if (LHS.DefType == ED_EXEC && RHS.DefType == ED_AND) {
        if (!DefComp.contains(LHS.LVN)) {
          if (!PhiVNs.contains(LHS.LVN))
            Incomplete = true;
          return std::nullopt;
        }
        ExecDef LHSDef = DefComp[LHS.LVN];
        if (LHSDef.DefType == ED_AND) {
          ExecDef RHSDef = RHS;
          // Make AND with negation RHS
          if (LHSDef.InvertRHS)
            std::swap(LHSDef, RHSDef);
          // Conditions must be opposite
          if (LHSDef.InvertRHS == RHSDef.InvertRHS)
            return std::nullopt;
          // Flip order of LHS operands to simplify match
          if (LHSDef.RVN == RHSDef.LVN)
            std::swap(LHSDef.LVN, LHSDef.RVN);
          if (LHSDef.LVN == RHSDef.LVN && LHSDef.RVN == RHSDef.RVN &&
              LHSDef.Ext == RHSDef.Ext)
            return ExecDef(ED_EXEC, RHSDef.LVN);
        }
      }
    }

    if (DT == ED_XOR) {
      // Fold XOR X (AND X Y) -> AND X ~Y
      if (RHS.DefType == ED_AND) {
        if (RHS.LVN == LHS.LVN)
          return ExecDef(ED_AND, LHS.LVN, RHS.RVN, /*Invert=*/!RHS.InvertRHS,
                         /*Src=*/RHS.Ext);
        if (RHS.RVN == LHS.LVN)
          return ExecDef(ED_AND, LHS.LVN, RHS.LVN, /*Invert=*/true,
                         /*Src=*/RHS.Ext);
      }
    }

    return std::nullopt;
  };

  std::function<ExecDef(MachineInstr *, bool, bool &)> resolveExecComp =
      [&](MachineInstr *MI, bool Root, bool &Incomplete) -> ExecDef {
    auto Opcode = MI->getOpcode();

    LLVM_DEBUG(dbgs() << "resolveExecComp: " << *MI);

    // Follow basic operations that copy values.
    if (Opcode == LMC->MovOpc || Opcode == LMC->MovTermOpc || Opcode == AMDGPU::COPY) {
      auto &SrcOp = MI->getOperand(1);
      if (!SrcOp.isReg())
        return ExecDef(ED_EXT, MI);
      if (SrcOp.getReg() == LMC->ExecReg)
        return ExecDef(ED_EXEC, VNs.findLeader(InstrVN[MI]));
      auto *Src = TRI->findReachingDef(SrcOp.getReg(), SrcOp.getSubReg(), *MI,
                                       *MRI, LIS);
      if (!Src)
        return ExecDef(ED_EXT, MI);
      return resolveExecComp(Src, false, Incomplete);
    }

    // Decompose: and/xor/or
    if (Opcode == LMC->AndOpc || Opcode == LMC->AndTermOpc || Opcode == LMC->AndN2Opc ||
        Opcode == LMC->AndN2TermOpc || Opcode == LMC->XorOpc || Opcode == LMC->XorTermOpc ||
        Opcode == LMC->OrOpc) {
      auto &LHSOp = MI->getOperand(1);
      auto &RHSOp = MI->getOperand(2);
      if (!LHSOp.isReg() || !RHSOp.isReg())
        return ExecDef(ED_EXT, MI);

      const unsigned ExecVN = VNs.findLeader(InstrVN[MI]);
      ExecDef LHS, RHS;
      if (LHSOp.getReg() == LMC->ExecReg) {
        LHS = ExecDef(ED_EXEC, ExecVN);
      } else {
        auto *Src = TRI->findReachingDef(LHSOp.getReg(), LHSOp.getSubReg(), *MI,
                                         *MRI, LIS);
        if (!Src)
          return ExecDef(ED_EXT, MI);
        LHS = resolveExecComp(Src, false, Incomplete);
      }
      if (RHSOp.getReg() == LMC->ExecReg) {
        RHS = ExecDef(ED_EXEC, ExecVN);
      } else {
        auto *Src = TRI->findReachingDef(RHSOp.getReg(), RHSOp.getSubReg(), *MI,
                                         *MRI, LIS);
        if (!Src)
          return ExecDef(ED_EXT, MI);
        RHS = resolveExecComp(Src, false, Incomplete);
      }

      LLVM_DEBUG(dbgs() << "  LHS: "; LHS.print());
      LLVM_DEBUG(dbgs() << "  RHS: "; RHS.print());

      // Operation should always involve one value of EXEC
      if (RHS.DefType == ED_EXEC)
        std::swap(LHS, RHS);
      if (LHS.DefType != ED_EXEC)
        return ExecDef(ED_EXT, MI);

      ExecDefType DefType = ED_UNKNOWN;
      bool InvertRHS = false;
      if (Opcode == LMC->AndOpc || Opcode == LMC->AndTermOpc) {
        DefType = ED_AND;
      } else if (Opcode == LMC->AndN2Opc || Opcode == LMC->AndN2TermOpc) {
        DefType = ED_AND;
        InvertRHS = true;
      } else if (Opcode == LMC->XorOpc || Opcode == LMC->XorTermOpc) {
        DefType = ED_XOR;
      } else if (Opcode == LMC->OrOpc) {
        DefType = ED_OR;
      }

      bool IsSimple = RHS.DefType == ED_EXEC || RHS.DefType == ED_EXT;
      if (IsSimple)
        return ExecDef(DefType, LHS.LVN, RHS.LVN, /*Invert=*/InvertRHS,
                       /*Src=*/RHS.Ext);

      auto Simplified = simplifyExecComp(DefType, LHS, RHS, Incomplete);
      if (Simplified.has_value())
        return *Simplified;
    }

    // Handle operations that manipulate and save EXEC simultaneously
    if (Opcode == LMC->OrSaveExecOpc || Opcode == LMC->AndSaveExecOpc ||
        Opcode == LMC->AndSaveExecTermOpc) {
      const unsigned ExecVN = VNs.findLeader(InstrVN[MI]);
      if (!Root)
        return ExecDef(ED_EXEC, ExecVN);
      auto &SrcOp = MI->getOperand(1);
      auto *Src = TRI->findReachingDef(SrcOp.getReg(), SrcOp.getSubReg(), *MI,
                                       *MRI, LIS);
      if (!Src)
        return ExecDef(ED_EXT, MI);

      ExecDefType DefType = Opcode == LMC->OrSaveExecOpc ? ED_OR : ED_AND;
      ExecDef RHS = resolveExecComp(Src, false, Incomplete);
      LLVM_DEBUG(dbgs() << "  RHS: "; RHS.print());

      if (RHS.DefType == ED_EXEC || RHS.DefType == ED_EXT)
        return ExecDef(DefType, ExecVN, RHS.LVN, /*Invert=*/false,
                       /*Src*/ RHS.Ext);

      ExecDef LHS = ExecDef(ED_EXEC, ExecVN);
      auto Simplified = simplifyExecComp(DefType, LHS, RHS, Incomplete);
      if (Simplified.has_value())
        return *Simplified;
    }

    // Something unknown: consider it an external condition
    return ExecDef(ED_EXT, MI);
  };

  SmallVector<unsigned> Recompute;
  for (unsigned VN = 0; VN < NextVN; ++VN) {
    if (!VNDefRev.contains(VN))
      continue;
    MachineInstr *MI = VNDefRev[VN];
    bool Incomplete = false;
    DefComp[VN] = resolveExecComp(MI, /*Root=*/true, Incomplete);
    LLVM_DEBUG(dbgs() << "Def " << VN << ": "; DefComp[VN].print());
    if (Incomplete)
      Recompute.push_back(VN);
    if (DefComp[VN].DefType == ED_EXEC && DefComp[VN].LVN != VN) {
      LLVM_DEBUG(dbgs() << "Join " << VN << " " << DefComp[VN].LVN << "\n");
      VNs.join(VN, DefComp[VN].LVN);
    }
  }
  for (unsigned VN : Recompute) {
    MachineInstr *MI = VNDefRev[VN];
    bool Incomplete = false;
    DefComp[VN] = resolveExecComp(MI, /*Root=*/true, Incomplete);
    LLVM_DEBUG(dbgs() << "Def " << VN << ": "; DefComp[VN].print());
    if (DefComp[VN].DefType == ED_EXEC && DefComp[VN].LVN != VN) {
      LLVM_DEBUG(dbgs() << "Join " << VN << " " << DefComp[VN].LVN << "\n");
      VNs.join(VN, DefComp[VN].LVN);
    }
    assert(!Incomplete);
  }

  LLVM_DEBUG(printInfo());
}

unsigned AMDGPUExecMaskAnalysis::getPrincipleVN(MachineBasicBlock *MBB) {
  assert(BlockVN.contains(MBB));
  if (MBB->instr_begin() == MBB->instr_end())
    return VNs.findLeader(BlockVN[MBB]);
  // Note: skip block prologue
  auto I = MBB->SkipPHIsLabelsAndDebug(MBB->instr_begin());
  return VNs.findLeader(InstrVN[&*I]);
}
