#include "core/providers/cpu/tensor/reshape.h"

namespace Lotus {

//non-copying reshape kernel
REGISTER_KERNEL(KernelDef("Reshape")
                    .Domain(LotusIR::kOnnxDomain)
                    .SinceVersion(1, 2)
                    .Provider(LotusIR::kCpuExecutionProvider)
                    .TypeConstraint("T", DataTypeImpl::GetTensorType<float>())
                    .Alias(0, 0),
                Reshape<float>);

//copying reshape kernel
REGISTER_KERNEL(KernelDef("Reshape")
                    .Domain(LotusIR::kOnnxDomain)
                    .SinceVersion(1, 2)
                    .Provider(LotusIR::kCpuExecutionProvider)
                    .TypeConstraint("T", DataTypeImpl::GetTensorType<float>()),
                Reshape<float>);

}  // namespace Lotus