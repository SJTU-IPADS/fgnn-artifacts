#ifndef SAMGRAPH_CPU_DEVICE_H
#define SAMGRAPH_CPU_DEVICE_H

#include "../device.h"

namespace samgraph {
namespace common {
namespace cpu {

class CPUDevice final : public Device {
 public:
  void SetDevice(Context ctx) override;
  void *AllocDataSpace(Context ctx, size_t nbytes,
                       size_t alignment = kAllocAlignment) override;
  void FreeDataSpace(Context ctx, void *ptr) override;
  void *AllocWorkspace(Context ctx, size_t nbytes,
                       size_t scale = Constant::kAllocScale) override;
  void FreeWorkspace(Context ctx, void *ptr, size_t nbytes = 0) override;
  void CopyDataFromTo(const void *from, size_t from_offset, void *to,
                      size_t to_offset, size_t nbytes, Context ctx_from,
                      Context ctx_to, StreamHandle stream) override;

  void StreamSync(Context ctx, StreamHandle stream) override;

  static const std::shared_ptr<CPUDevice> &Global();
};

}  // namespace cpu
}  // namespace common
}  // namespace samgraph

#endif  // SAMGRAPH_CPU_DEVICE_H
