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

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Target/TargetOptions.h"
#include "xla/backends/cpu/codegen/cpu_features.h"
#include "xla/backends/cpu/codegen/ir_compiler.h"
#include "xla/backends/cpu/codegen/jit_compiler.h"
#include "xla/backends/cpu/codegen/target_machine_features.h"
#include "xla/debug_options_flags.h"
#include "xla/hlo/analysis/alias_info.h"
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_module.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/hlo/ir/hlo_schedule.h"
#include "xla/hlo/tools/hlo_opt/opt_lib.h"
#include "xla/hlo/transforms/host_offloader.h"
#include "xla/hlo/transforms/simplifiers/hlo_memory_scheduler.h"
#include "xla/hlo/translate/hlo_to_mhlo/hlo_to_mlir_hlo.h"
#include "xla/service/batchnorm_expander.h"
#include "xla/service/change_op_data_type.h"
#include "xla/service/copy_insertion.h"
#include "xla/service/cpu/conv_canonicalization.h"
#include "xla/service/cpu/cpu_compiler.h"
#include "xla/service/cpu/cpu_executable.h"
#include "xla/service/cpu/cpu_instruction_fusion.h"
#include "xla/service/cpu/cpu_layout_assignment.h"
#include "xla/service/cpu/dot_op_emitter.h"
#include "xla/service/cpu/executable.pb.h"
#include "xla/service/cpu/parallel_task_assignment.h"
#include "xla/service/dynamic_dimension_inference.h"
#include "xla/service/dynamic_padder.h"
#include "xla/service/executable.h"
#include "xla/service/hlo.pb.h"
#include "xla/service/hlo_execution_profile.h"
#include "xla/service/hlo_module_config.h"
#include "xla/service/hlo_profile_printer_data.pb.h"
#include "xla/service/llvm_ir/llvm_util.h"
#include "xla/service/sharding_propagation.h"
#include "xla/service/spmd/stateful_rng_spmd_partitioner.h"
#include "xla/service/transpose_folding.h"
#include "xla/shape_util.h"
#include "xla/stream_executor/platform/initialize.h"
#include "xla/tools/hlo_opt/compiled_opt_lib.h"
#include "xla/tsl/platform/logging.h"  // IWYU pragma: keep
#include "xla/tsl/platform/statusor.h"
#include "xla/util.h"
#include "xla/xla.pb.h"
#include "xla/xla_data.pb.h"
#include "tsl/platform/cpu_info.h"

namespace xla {

namespace {

class CpuOptProvider : public CompiledOptProvider {
 public:
  CpuOptProvider() : CompiledOptProvider() {}

  absl::StatusOr<std::optional<std::string>> GenerateStage(
      std::unique_ptr<HloModule> module, absl::string_view s) override {
    if (s == "llvm-before-optimizations") {
      TF_ASSIGN_OR_RETURN(std::unique_ptr<Executable> executable,
                          GetExecutable(std::move(module)));
      return static_cast<cpu::CpuExecutable*>(executable.get())
          ->ir_module_string();
    }
    return CompiledOptProvider::GenerateStage(std::move(module), s);
  }

  std::set<std::string> SupportedStages() override {
    std::set<std::string> supported = CompiledOptProvider::SupportedStages();
    supported.insert({"llvm-before-optimizations"});
    return supported;
  }

  std::string GetPlatformName() override { return "cpu"; }

  std::string GetRegisteredPassNames() override {
    return GetRegisteredPassNamesHelper(pass_registry_);
  }

  //////////////////////////////////////////////////////////////////////////////
  // Registration of CPU-specific HLO Passes                                  //
  //////////////////////////////////////////////////////////////////////////////
  void RegisterProviderPasses(HloModule& module) override {
    // initialize all needed to extract configs for pass registration
    // and pass it to the register function
    DebugOptions debug_opts = GetDebugOptionsFromFlags();
    auto executor = GetExecutor();
    HloModuleConfig module_config = module.config();
    BufferValue::SizeFunction size_func = [](const BufferValue& buffer) {
      const Shape& shape = buffer.shape();
      // On the cpu, opaques are pointers.
      if (shape.IsOpaque()) {
        return static_cast<int64_t>(sizeof(void*));
      }
      return ShapeUtil::ByteSizeOf(shape, sizeof(void*));
    };
    absl::StatusOr<std::unique_ptr<llvm::TargetMachine>> jit_target_machine =
        cpu::IrCompiler::InferTargetMachine(
            CompilerTargetOptions(module_config),
            CodeGenOptLevel(module_config),
            cpu::CpuFeatureFromString(
                module_config.debug_options().xla_cpu_max_isa()));
    if (!jit_target_machine.ok()) {
      LOG(ERROR) << "Failed to infer target machine: "
                 << jit_target_machine.status();
      return;
    }

    cpu::TargetMachineFeatures target_machine_features(
        jit_target_machine->get());

    RegisterPass<ShardingPropagation>(
        /*is_spmd=*/true,
        /*propagate_metadata=*/false,
        module_config.allow_spmd_sharding_propagation_to_output(),
        module_config.allow_spmd_sharding_propagation_to_parameters(),
        /*cse_prevention_only=*/false,
        /*sharding_helper=*/nullptr);

    RegisterPass<spmd::StatefulRngSpmdPartitioner>(
        module_config.num_partitions(), module_config.replica_count());
    RegisterPass<BatchNormExpander>(
        /*rewrite_training_op=*/true,
        /*rewrite_inference_op=*/true,
        /*rewrite_grad_op=*/true);
    auto dynamic_padder_options = DynamicPadderOptions();
    dynamic_padder_options.shape_check_mode =
        DynamicDimensionInference::ShapeCheckMode::kIgnore;
    RegisterPass<DynamicPadder>(dynamic_padder_options);
    RegisterPass<ChangeOpDataType>(
        F16, F32, HloPredicateIsOp<HloOpcode::kDot, HloOpcode::kConvolution>);
    RegisterPass<TransposeFolding>(
        [&](const HloInstruction& dot,
            int64_t operand) -> absl::StatusOr<bool> {
          if (DotImplementationCanHandleTranspose(
                  dot, target_machine_features, /*allow_runtime_calls=*/true)) {
            return TransposeFolding::IsRowColumnTransposeDotOperand(dot,
                                                                    operand);
          }
          return false;
        },
        TransposeFolding::NeverFoldTranspose);
    RegisterPass<cpu::ConvCanonicalization>(&target_machine_features);
    RegisterPass<HloMemoryScheduler>(alias_info_.get(), size_func);
    RegisterPass<HostOffloader>(alias_info_.get());

    // Fails to register if module does not have entry computation layout
    if (module.config().has_entry_computation_layout()) {
      RegisterPass<cpu::CpuLayoutAssignment>(
          module.mutable_entry_computation_layout(), &target_machine_features,
          nullptr);
    }

    const int max_parallelism =
        module_config.intra_op_parallelism_threads() > 0
            ? module_config.intra_op_parallelism_threads()
            : tsl::port::NumSchedulableCPUs();
    RegisterPass<cpu::ParallelTaskAssigner>(max_parallelism,
                                            cpu::CpuExecutable::ShapeSizeBytes,
                                            &target_machine_features);
    RegisterPass<cpu::CpuInstructionFusion>();
    RegisterPass<CopyInsertion>(alias_info_.get());
  }

 private:
  llvm::CodeGenOptLevel CodeGenOptLevel(const HloModuleConfig& module_config) {
    VLOG(2) << "backend_optimization_level: "
            << module_config.debug_options().xla_backend_optimization_level();
    switch (module_config.debug_options().xla_backend_optimization_level()) {
      case 1:
        return llvm::CodeGenOptLevel::Less;
      case 2:
        return llvm::CodeGenOptLevel::Default;
      case 3:
        return llvm::CodeGenOptLevel::Aggressive;
      default:
        return llvm::CodeGenOptLevel::None;
    }
  }

  llvm::TargetOptions CompilerTargetOptions(
      const HloModuleConfig& module_config) {
    llvm::TargetOptions target_options;
    // Always allow FMA fusion. This increases precision instead of decreasing
    // it.
    target_options.AllowFPOpFusion = llvm::FPOpFusion::Fast;
    return target_options;
  }
};

}  // namespace
}  // namespace xla

STREAM_EXECUTOR_REGISTER_MODULE_INITIALIZER(cpu_opt_provider, {
  xla::OptProvider::RegisterForPlatform(
      "cpu", std::make_unique<xla::CpuOptProvider>());
});
