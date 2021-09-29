#include <cassert>

#include "../common.h"
#include "../logging.h"
#include "../run_config.h"
#include "cpu_function.h"

namespace samgraph {
namespace common {
namespace cpu {

namespace {

template <typename T>
void cpu_extract(void *dst, const void *src, const IdType *index,
                 size_t num_index, size_t dim) {
  T *dst_data = reinterpret_cast<T *>(dst);
  const T *src_data = reinterpret_cast<const T *>(src);

#pragma omp parallel for num_threads(RunConfig::omp_thread_num)
  for (size_t i = 0; i < num_index; ++i) {
    size_t src_index = index[i];
#pragma omp simd
    for (size_t j = 0; j < dim; j++) {
      dst_data[i * dim + j] = src_data[src_index * dim + j];
    }
  }
}

template <typename T>
void cpu_mock_extract(void *dst, const void *src, const IdType *index,
                 size_t num_index, size_t dim) {
  T *dst_data = reinterpret_cast<T *>(dst);
  const T *src_data = reinterpret_cast<const T *>(src);
  size_t idx_mock_mask = (1ull << RunConfig::option_empty_feat) - 1;

#pragma omp parallel for num_threads(RunConfig::omp_thread_num)
  for (size_t i = 0; i < num_index; ++i) {
    size_t src_index = index[i] & idx_mock_mask;
#pragma omp simd
    for (size_t j = 0; j < dim; j++) {
      dst_data[i * dim + j] = src_data[src_index * dim + j];
    }
  }
}

}  // namespace

void CPUExtract(void *dst, const void *src, const IdType *index,
                size_t num_index, size_t dim, DataType dtype) {
  switch (dtype) {
    case kF32:
      cpu_extract<float>(dst, src, index, num_index, dim);
      break;
    case kF64:
      cpu_extract<double>(dst, src, index, num_index, dim);
      break;
    case kF16:
      cpu_extract<short>(dst, src, index, num_index, dim);
      break;
    case kU8:
      cpu_extract<uint8_t>(dst, src, index, num_index, dim);
      break;
    case kI32:
      cpu_extract<int32_t>(dst, src, index, num_index, dim);
      break;
    case kI64:
      cpu_extract<int64_t>(dst, src, index, num_index, dim);
      break;
    default:
      CHECK(0);
  }
}

void CPUMockExtract(void *dst, const void *src, const IdType *index,
                    size_t num_index, size_t dim, DataType dtype) {
  switch (dtype) {
    case kF32:
      cpu_mock_extract<float>(dst, src, index, num_index, dim);
      break;
    case kF64:
      cpu_mock_extract<double>(dst, src, index, num_index, dim);
      break;
    case kF16:
      cpu_mock_extract<short>(dst, src, index, num_index, dim);
      break;
    case kU8:
      cpu_mock_extract<uint8_t>(dst, src, index, num_index, dim);
      break;
    case kI32:
      cpu_mock_extract<int32_t>(dst, src, index, num_index, dim);
      break;
    case kI64:
      cpu_mock_extract<int64_t>(dst, src, index, num_index, dim);
      break;
    default:
      CHECK(0);
  }
}
}  // namespace cpu
}  // namespace common
}  // namespace samgraph