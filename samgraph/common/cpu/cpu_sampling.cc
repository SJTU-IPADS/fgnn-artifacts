#include <algorithm>
#include <random>

#include "../common.h"
#include "../config.h"
#include "cpu_function.h"

namespace samgraph {
namespace common {
namespace cpu {

namespace {

IdType RandomID(const IdType &min, const IdType &max) {
  static thread_local std::mt19937 generator;
  std::uniform_int_distribution<IdType> distribution(min, max);
  return distribution(generator);
}

}  // namespace

void CpuFunction::CpuSample(const IdType *const indptr,
                            const IdType *const indices,
                            const IdType *const input, const size_t num_input,
                            IdType *output_src, IdType *output_dst,
                            size_t *num_ouput, const size_t fanout) {
  bool all_has_fanout = true;

#pragma omp parallel for num_threads(Config::kOmpThreadNum) reduction  (&&:all_has_fanout)
  for (size_t i = 0; i < num_input; ++i) {
    const IdType rid = input[i];
    const IdType off = indptr[rid];
    const IdType len = indptr[rid + 1] - off;

    all_has_fanout = all_has_fanout && (len >= fanout);

    if (len <= fanout) {
      size_t j = 0;
      for (; j < len; ++j) {
        output_src[i * fanout + j] = rid;
        output_dst[i * fanout + j] = indices[off + j];
      }

      for (; j < fanout; ++j) {
        output_src[i * fanout + j] = Config::kEmptyKey;
        output_dst[i * fanout + j] = Config::kEmptyKey;
      }
    } else {
      // reservoir algorithm
      // time: O(population), space: O(num)
      for (size_t j = 0; j < fanout; ++j) {
        output_src[i * fanout + j] = rid;
        output_dst[i * fanout + j] = indices[off + j];
      }

      for (size_t j = fanout; j < len; ++j) {
        const IdType k = RandomID(0, j + 1);
        if (k < fanout) {
          output_dst[i * fanout + k] = indices[off + j];
        }
      }
    }
  }

  if (!all_has_fanout) {
    IdType *output_src_end =
        std::remove_if(output_src, output_src + num_input * fanout,
                       [](IdType num) { return num == Config::kEmptyKey; });
    std::remove_if(output_dst, output_dst + num_input * fanout,
                   [](IdType num) { return num == Config::kEmptyKey; });

    *num_ouput = output_src_end - output_src;
  } else {
    *num_ouput = num_input * fanout;
  }
}

}  // namespace cpu
}  // namespace common
}  // namespace samgraph
