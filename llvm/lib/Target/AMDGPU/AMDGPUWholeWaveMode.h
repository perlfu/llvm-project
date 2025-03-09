//===- AMDGPUWholeWaveMode.h ------------------------------------*- C++- *-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AMDGPU_AMDGPUWHOLEWAVEMODE_H
#define LLVM_LIB_TARGET_AMDGPU_AMDGPUWHOLEWAVEMODE_H

#include "llvm/CodeGen/MachinePassManager.h"

namespace llvm {
class AMDGPUWholeWaveModePass : public PassInfoMixin<AMDGPUWholeWaveModePass> {
public:
  PreservedAnalyses run(MachineFunction &MF,
                        MachineFunctionAnalysisManager &MFAM);

  /*
  MachineFunctionProperties getClearedProperties() const {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::IsSSA);
  }
  */
};
} // namespace llvm

#endif // LLVM_LIB_TARGET_AMDGPU_AMDGPUWHOLEWAVEMODE_H
