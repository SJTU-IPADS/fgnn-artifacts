#include <cassert>
#include <chrono>
#include <numeric>

#include "../common.h"
#include "../constant.h"
#include "../device.h"
#include "../logging.h"
#include "../run_config.h"
#include "../timer.h"
#include "cuda_random_states.h"

namespace samgraph {
namespace common {
namespace cuda {

namespace {

__global__ void init_random_states(curandState *states, size_t num,
                                   unsigned long seed) {
  size_t threadId = threadIdx.x + blockIdx.x * blockDim.x;
  if (threadId <= num) {
    curand_init(seed, threadId, 0, &states[threadId]);
  }
}

}  // namespace

GPURandomStates::GPURandomStates(SampleType sample_type,
                                 const std::vector<size_t> &fanout,
                                 const size_t batch_size, Context ctx) {
  _ctx = ctx;
  auto device = Device::Get(_ctx);

  switch (sample_type) {
    case kKHop0:
      _num_states = PredictNumNodes(batch_size, fanout, fanout.size() - 1);
      break;
    case kKHop1:
      _num_states = PredictNumNodes(batch_size, fanout, fanout.size());
      _num_states = Min(_num_states, Constant::kKHop1MaxThreads);
      break;
    case kWeightedKHop:
      _num_states = PredictNumNodes(batch_size, fanout, fanout.size());
      _num_states = Min(_num_states, Constant::kWeightedKHopMaxThreads);
      break;
    case kRandomWalk:
    default:
      CHECK(0);
  }

  _states = static_cast<curandState *>(
      device->AllocDataSpace(_ctx, sizeof(curandState) * _num_states));

  const dim3 grid(
      RoundUpDiv(_num_states, static_cast<size_t>(Constant::kCudaBlockSize)));
  const dim3 block(Constant::kCudaBlockSize);

  unsigned long seed =
      std::chrono::system_clock::now().time_since_epoch().count();
  init_random_states<<<grid, block>>>(_states, _num_states, seed);
}

GPURandomStates::~GPURandomStates() {
  auto device = Device::Get(_ctx);
  device->FreeDataSpace(_ctx, _states);
}

}  // namespace cuda
}  // namespace common
}  // namespace samgraph
