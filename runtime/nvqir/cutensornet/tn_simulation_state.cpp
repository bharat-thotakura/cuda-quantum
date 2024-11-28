/*******************************************************************************
 * Copyright (c) 2022 - 2024 NVIDIA Corporation & Affiliates.                  *
 * All rights reserved.                                                        *
 *                                                                             *
 * This source code and the accompanying materials are made available under    *
 * the terms of the Apache License 2.0 which accompanies this distribution.    *
 ******************************************************************************/

#include "tn_simulation_state.h"
#include "cudaq/utils/cudaq_utils.h"
#include <cuComplex.h>

namespace nvqir {
int deviceFromPointer(void *ptr) {
  cudaPointerAttributes attributes;
  HANDLE_CUDA_ERROR(cudaPointerGetAttributes(&attributes, ptr));
  return attributes.device;
}
std::size_t TensorNetSimulationState::getNumQubits() const {
  return m_state->getNumQubits();
}

TensorNetSimulationState::TensorNetSimulationState(
    std::unique_ptr<TensorNetState> inState, ScratchDeviceMem &inScratchPad,
    cutensornetHandle_t cutnHandle, std::mt19937 &randomEngine)
    : m_state(std::move(inState)), scratchPad(inScratchPad),
      m_cutnHandle(cutnHandle), m_randomEngine(randomEngine) {}

TensorNetSimulationState::~TensorNetSimulationState() {}

std::complex<double>
TensorNetSimulationState::overlap(const cudaq::SimulationState &other) {
  const cudaq::SimulationState *const otherStatePtr = &other;
  const TensorNetSimulationState *const tnOther =
      dynamic_cast<const TensorNetSimulationState *>(otherStatePtr);
  LOG_API_TIME();
  if (!tnOther)
    throw std::runtime_error("[tensornet state] Computing overlap with other "
                             "types of state is not supported.");
  auto tensorOps = tnOther->m_state->m_tensorOps;
  // Compute <bra|ket> by conjugating the entire |bra> tensor network.
  // Reverse them
  std::reverse(tensorOps.begin(), tensorOps.end());
  for (auto &op : tensorOps) {
    op.isAdjoint = !op.isAdjoint;
    if (!op.isUnitary) {
      // For non-unitary ops, i.e., projectors, we need to do a transpose to
      // reverse the leg connection.
      const auto dim = (1 << op.targetQubitIds.size());
      // FIXME: perform this in device memory.
      Eigen::MatrixXcd mat(dim, dim);
      HANDLE_CUDA_ERROR(cudaMemcpy(mat.data(), op.deviceData,
                                   mat.size() * sizeof(std::complex<double>),
                                   cudaMemcpyDeviceToHost));
      mat.transposeInPlace();
      HANDLE_CUDA_ERROR(cudaMemcpy(op.deviceData, mat.data(),
                                   mat.size() * sizeof(std::complex<double>),
                                   cudaMemcpyHostToDevice));
    }
  }
  // Append them to ket
  // Note: we clone a new ket tensor network to keep this ket as-is.
  const auto nbQubits = std::max(getNumQubits(), other.getNumQubits());
  const std::vector<int64_t> qubitDims(nbQubits, 2);
  cutensornetState_t tempQuantumState;
  auto &cutnHandle = m_state->m_cutnHandle;
  HANDLE_CUTN_ERROR(cutensornetCreateState(
      cutnHandle, CUTENSORNET_STATE_PURITY_PURE, nbQubits, qubitDims.data(),
      CUDA_C_64F, &tempQuantumState));

  int64_t tensorId = 0;
  // Append ket-side gate tensors + conjugated (reverse + adjoint) bra-side
  // tensors
  auto allTensorOps = m_state->m_tensorOps;
  allTensorOps.insert(allTensorOps.end(), tensorOps.begin(), tensorOps.end());

  for (auto &op : allTensorOps) {
    if (op.controlQubitIds.empty()) {
      HANDLE_CUTN_ERROR(cutensornetStateApplyTensorOperator(
          cutnHandle, tempQuantumState, op.targetQubitIds.size(),
          op.targetQubitIds.data(), op.deviceData, nullptr, /*immutable*/ 1,
          /*adjoint*/ static_cast<int32_t>(op.isAdjoint),
          /*unitary*/ static_cast<int32_t>(op.isUnitary), &tensorId));
    } else {
      HANDLE_CUTN_ERROR(cutensornetStateApplyControlledTensorOperator(
          cutnHandle, tempQuantumState,
          /*numControlModes=*/op.controlQubitIds.size(),
          /*stateControlModes=*/op.controlQubitIds.data(),
          /*stateControlValues=*/nullptr,
          /*numTargetModes*/ op.targetQubitIds.size(),
          /*stateTargetModes*/ op.targetQubitIds.data(), op.deviceData, nullptr,
          /*immutable*/ 1,
          /*adjoint*/ static_cast<int32_t>(op.isAdjoint),
          /*unitary*/ static_cast<int32_t>(op.isUnitary), &tensorId));
    }
  }

  // Cap off with all zero projection (initial state of bra)
  std::vector<int32_t> projectedModes(nbQubits);
  std::iota(projectedModes.begin(), projectedModes.end(), 0);
  std::vector<int64_t> projectedModeValues(nbQubits, 0);
  void *d_overlap;
  HANDLE_CUDA_ERROR(cudaMalloc(&d_overlap, sizeof(std::complex<double>)));
  // Create the quantum state amplitudes accessor
  cutensornetStateAccessor_t accessor;
  {
    ScopedTraceWithContext("cutensornetCreateAccessor");
    HANDLE_CUTN_ERROR(cutensornetCreateAccessor(
        cutnHandle, tempQuantumState, projectedModes.size(),
        projectedModes.data(), nullptr, &accessor));
  }

  const int32_t numHyperSamples =
      8; // desired number of hyper samples used in the tensor network
         // contraction path finder
  {
    ScopedTraceWithContext("cutensornetAccessorConfigure");
    HANDLE_CUTN_ERROR(cutensornetAccessorConfigure(
        cutnHandle, accessor, CUTENSORNET_ACCESSOR_OPT_NUM_HYPER_SAMPLES,
        &numHyperSamples, sizeof(numHyperSamples)));
  }
  // Prepare the quantum state amplitudes accessor
  cutensornetWorkspaceDescriptor_t workDesc;
  HANDLE_CUTN_ERROR(
      cutensornetCreateWorkspaceDescriptor(cutnHandle, &workDesc));
  {
    ScopedTraceWithContext("cutensornetAccessorPrepare");
    HANDLE_CUTN_ERROR(cutensornetAccessorPrepare(
        cutnHandle, accessor, scratchPad.scratchSize, workDesc, 0));
  }

  // Attach the workspace buffer
  int64_t worksize = 0;
  HANDLE_CUTN_ERROR(cutensornetWorkspaceGetMemorySize(
      cutnHandle, workDesc, CUTENSORNET_WORKSIZE_PREF_RECOMMENDED,
      CUTENSORNET_MEMSPACE_DEVICE, CUTENSORNET_WORKSPACE_SCRATCH, &worksize));
  if (worksize <= static_cast<int64_t>(scratchPad.scratchSize)) {
    HANDLE_CUTN_ERROR(cutensornetWorkspaceSetMemory(
        cutnHandle, workDesc, CUTENSORNET_MEMSPACE_DEVICE,
        CUTENSORNET_WORKSPACE_SCRATCH, scratchPad.d_scratch, worksize));
  } else {
    throw std::runtime_error("ERROR: Insufficient workspace size on Device!");
  }

  // Compute the quantum state amplitudes
  std::complex<double> stateNorm{0.0, 0.0};
  // Result overlap (host data)
  std::complex<double> h_overlap{0.0, 0.0};
  {
    ScopedTraceWithContext("cutensornetAccessorCompute");
    HANDLE_CUTN_ERROR(cutensornetAccessorCompute(
        cutnHandle, accessor, projectedModeValues.data(), workDesc, d_overlap,
        static_cast<void *>(&stateNorm), 0));
  }
  HANDLE_CUDA_ERROR(cudaMemcpy(&h_overlap, d_overlap,
                               sizeof(std::complex<double>),
                               cudaMemcpyDeviceToHost));
  // Free resources
  HANDLE_CUDA_ERROR(cudaFree(d_overlap));
  HANDLE_CUTN_ERROR(cutensornetDestroyWorkspaceDescriptor(workDesc));
  HANDLE_CUTN_ERROR(cutensornetDestroyAccessor(accessor));
  HANDLE_CUTN_ERROR(cutensornetDestroyState(tempQuantumState));

  return std::abs(h_overlap);
}

std::complex<double>
TensorNetSimulationState::getAmplitude(const std::vector<int> &basisState) {
  if (getNumQubits() != basisState.size())
    throw std::runtime_error(
        fmt::format("[tensornet-state] getAmplitude with an invalid number "
                    "of bits in the "
                    "basis state: expected {}, provided {}.",
                    getNumQubits(), basisState.size()));
  if (std::any_of(basisState.begin(), basisState.end(),
                  [](int x) { return x != 0 && x != 1; }))
    throw std::runtime_error(
        "[tensornet-state] getAmplitude with an invalid basis state: only "
        "qubit state (0 or 1) is supported.");

  if (basisState.empty())
    throw std::runtime_error("[tensornet-state] Empty basis state.");

  if (m_state->getNumQubits() <= g_maxQubitsForStateContraction) {
    // If this is the first time, cache the state.
    if (m_contractedStateVec.empty())
      m_contractedStateVec = m_state->getStateVector();
    assert(m_contractedStateVec.size() == (1ULL << m_state->getNumQubits()));
    const std::size_t idx = std::accumulate(
        std::make_reverse_iterator(basisState.end()),
        std::make_reverse_iterator(basisState.begin()), 0ull,
        [](std::size_t acc, int bit) { return (acc << 1) + bit; });
    return m_contractedStateVec[idx];
  }

  std::vector<int32_t> projectedModes(m_state->getNumQubits());
  std::iota(projectedModes.begin(), projectedModes.end(), 0);
  std::vector<int64_t> projectedModeValues;
  projectedModeValues.assign(basisState.begin(), basisState.end());
  auto subStateVec =
      m_state->getStateVector(projectedModes, projectedModeValues);
  assert(subStateVec.size() == 1);
  return subStateVec[0];
}

cudaq::SimulationState::Tensor
TensorNetSimulationState::getTensor(std::size_t tensorIdx) const {
  if (tensorIdx >= getNumTensors())
    throw std::out_of_range("Invalid tensor index");
  cudaq::SimulationState::Tensor tensor;
  auto &opTensor = m_state->m_tensorOps[tensorIdx];
  tensor.data = opTensor.deviceData;
  std::vector<std::size_t> extents(2 * opTensor.targetQubitIds.size(), 2);
  tensor.extents = extents;
  tensor.fp_precision = getPrecision();
  return tensor;
}

std::vector<cudaq::SimulationState::Tensor>
TensorNetSimulationState::getTensors() const {
  std::vector<cudaq::SimulationState::Tensor> tensors;
  tensors.reserve(m_state->m_tensorOps.size());

  for (auto &op : m_state->m_tensorOps) {
    cudaq::SimulationState::Tensor tensor;
    tensor.data = op.deviceData;
    std::vector<std::size_t> extents(2 * op.targetQubitIds.size(), 2);
    tensor.extents = extents;
    tensor.fp_precision = getPrecision();
    tensors.emplace_back(std::move(tensor));
  }
  return tensors;
}

std::size_t TensorNetSimulationState::getNumTensors() const {
  return m_state->m_tensorOps.size();
}

std::unique_ptr<cudaq::SimulationState>
TensorNetSimulationState::createFromSizeAndPtr(std::size_t size, void *ptr,
                                               std::size_t dataType) {
  if (dataType == cudaq::detail::variant_index<cudaq::state_data,
                                               cudaq::TensorStateData>()) {
    throw std::runtime_error(
        "Cannot create tensornet backend's simulation state with MPS tensors.");
  }
  std::vector<std::complex<double>> vec(
      reinterpret_cast<std::complex<double> *>(ptr),
      reinterpret_cast<std::complex<double> *>(ptr) + size);
  auto tensorNetState = TensorNetState::createFromStateVector(
      vec, scratchPad, m_cutnHandle, m_randomEngine);

  return std::make_unique<TensorNetSimulationState>(
      std::move(tensorNetState), scratchPad, m_cutnHandle, m_randomEngine);
}

void TensorNetSimulationState::destroyState() {
  cudaq::info("mps-state destroying state vector handle.");
  m_state.reset();
}

void TensorNetSimulationState::toHost(std::complex<double> *clientAllocatedData,
                                      std::size_t numElements) const {
  auto stateVec = m_state->getStateVector();
  if (stateVec.size() != numElements)
    throw std::runtime_error(fmt::format(
        "[TensorNetSimulationState] Dimension mismatch: expecting {} "
        "elements but providing an array of size {}.",
        stateVec.size(), numElements));
  for (std::size_t i = 0; i < numElements; ++i)
    clientAllocatedData[i] = stateVec[i];
}

void TensorNetSimulationState::dump(std::ostream &os) const {
  const auto printState = [&os](const auto &stateVec) {
    for (auto &t : stateVec)
      os << t << "\n";
  };

  if (!m_contractedStateVec.empty())
    printState(m_contractedStateVec);
  else
    printState(m_state->getStateVector());
}
} // namespace nvqir