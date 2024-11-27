/*******************************************************************************
 * Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

// clang-format off
// RUN: nvq++ %cpp_std --target anyon                    --emulate %s -o %t && %t | FileCheck %s
// RUN: nvq++ %cpp_std --target braket                   --emulate %s -o %t && %t | FileCheck %s
// RUN: nvq++ %cpp_std --target ionq                     --emulate %s -o %t && %t | FileCheck %s
// RUN: nvq++ %cpp_std --target iqm --iqm-machine Adonis --emulate %s -o %t && %t | FileCheck %s
// RUN: nvq++ %cpp_std --target oqc                      --emulate %s -o %t && %t | FileCheck %s
// RUN: nvq++ %cpp_std --target quantinuum               --emulate %s -o %t && %t | FileCheck %s
// RUN: nvq++ -std=c++17 --enable-mlir %s -o %t && %t | FileCheck %s

#include "cudaq.h"
#include <iostream>

int main() {

  auto swapKernel = []() __qpu__ {
    cudaq::qvector q(2);
    h(q[0]);
    cx(q[0], q[1]);
    mz(q);
  };

  auto counts = cudaq::sample(swapKernel);
  counts.dump(std::cout);

#ifndef SYNTAX_CHECK
  std::cout << counts.most_probable() << '\n';
#endif

  return 0;
}

// CHECK: 00 and 11