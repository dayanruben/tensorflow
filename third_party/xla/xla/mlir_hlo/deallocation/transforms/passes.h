/* Copyright 2023 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef MLIR_HLO_DEALLOCATION_TRANSFORMS_PASSES_H
#define MLIR_HLO_DEALLOCATION_TRANSFORMS_PASSES_H

#include <memory>

#include "mlir/Conversion/LLVMCommon/TypeConverter.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"

namespace mlir {
namespace deallocation {

// Pass to reuse buffers (hoisting, double buffering, dealloc/alloc
// coalescing).
std::unique_ptr<mlir::OperationPass<mlir::func::FuncOp>>
createBufferReusePass();

// Creates an instance of the BufferDeallocation pass to free all allocated
// buffers.
// TODO(b/380236304): Remove once migrated to one-shot bufferization.
std::unique_ptr<mlir::Pass> createBufferDeallocationPass();

#define GEN_PASS_REGISTRATION
#include "deallocation/transforms/passes.h.inc"

}  // namespace deallocation
}  // namespace mlir

#endif  // MLIR_HLO_DEALLOCATION_TRANSFORMS_PASSES_H
