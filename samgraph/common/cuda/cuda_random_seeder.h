#include <vector>
#include <curand_kernel.h>
namespace samgraph {
namespace common {
namespace cuda {

class GPURandomSeeder {
 public:
  GPURandomSeeder() { _states = nullptr; _initialize = false; };
  void Init(std::vector<int> fanouts, Context sampler_ctx,
                             StreamHandle sampler_stream, size_t batch_size);
  curandState* Get() { return _states; };
  size_t Size() { return _num_random; };
  static constexpr size_t maxSeedNum = ((5l * 1024 * 1024 + Constant::kCudaBlockSize - 1)
                                / Constant::kCudaBlockSize * Constant::kCudaBlockSize);

 private:
  // Whether the seeder is initialized
  bool _initialize;
  // random seeds list in CUDA for sampling
  curandState* _states;
  // random seeds size
  size_t _num_random;
};

} // cuda
} // common
} // samgraph
