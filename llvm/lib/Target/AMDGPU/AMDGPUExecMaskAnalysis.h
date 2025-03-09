//===- AMDGPUExecMaskAnalysis.h ---- analysis of exec register --*- C++ -*-===//
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

#ifndef LLVM_LIB_TARGET_AMDGPU_AMDGPUEXECMASKANALYSIS_H
#define LLVM_LIB_TARGET_AMDGPU_AMDGPUEXECMASKANALYSIS_H

#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstr.h"
#include <optional>

namespace llvm {

namespace AMDGPU {
class LaneMaskConstants;
} // namespace AMDGPU

class AMDGPUExecMaskAnalysis {
public:
  const unsigned UnknownVN = 0;
  const unsigned ZeroVN = 1;

private:
  MachineFunction *MF;
  const AMDGPU::LaneMaskConstants *LMC;
  DenseMap<MachineInstr *, unsigned> InstrVN;
  DenseMap<MachineBasicBlock *, unsigned> BlockVN;
  DenseMap<MachineInstr *, unsigned> VNDef;
  IntEqClasses VNs;

  bool isZeroVN(unsigned VN);
  std::optional<unsigned> getEdgeVN(MachineBasicBlock *Src,
                                    MachineBasicBlock *Dst);
  bool hasExecUse(MachineInstr *MI);
  MachineOperand *getNonExecUse(MachineInstr *MI);

public:
  AMDGPUExecMaskAnalysis() {}

  using FilterFn = std::function<bool(MachineInstr *)>;

  static bool defaultFilter(MachineInstr *MI) { return true; }

  void analyze(MachineFunction &TheMF, LiveIntervals *LIS,
               FilterFn Filter = defaultFilter);
  void clear();
  void printInfo();

  // Return VN for a block after any initial EXEC manipulation
  unsigned getPrincipleVN(MachineBasicBlock *MBB);
};

} // namespace llvm
#endif // LLVM_LIB_TARGET_AMDGPU_AMDGPUEXECMASKANALYSIS_H
