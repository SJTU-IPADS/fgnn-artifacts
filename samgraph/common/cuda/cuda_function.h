#ifndef SAMGRAPH_CUDA_FUNCTION_H
#define SAMGRAPH_CUDA_FUNCTION_H

#include <curand_kernel.h>

#include "../common.h"
#include "cuda_hashtable.h"

namespace samgraph {
namespace common {
namespace cuda {

void GPUSample(const IdType *indptr, const IdType *indices, const IdType *input,
               const size_t num_input, const size_t fanout, IdType *out_src,
               IdType *out_dst, size_t *num_out, Context ctx,
               StreamHandle stream, uint64_t task_key);

void GPUWeightedSample(const IdType *indptr, const IdType *indices,
                       const IdType *input, const size_t num_input,
                       const size_t fanout, IdType *out_src, IdType *out_dst,
                       size_t *num_out, Context ctx, StreamHandle stream,
                       uint64_t task_key);

void GPUNextdoorSample(const IdType *indptr, const IdType *indices,
                       const IdType *input, const size_t num_input,
                       const size_t fanout, IdType *out_src, IdType *out_dst,
                       size_t *num_out, Context ctx, StreamHandle stream,
                       uint64_t task_key, curandState *states,
                       size_t num_seeds);

void GPURandomWalkSample(const IdType *indptr, const IdType *indices,
                         const IdType *input, const size_t num_input,
                         const size_t fanout, IdType *out_src, IdType *out_dst,
                         size_t *num_out, Context ctx, StreamHandle stream,
                         uint64_t task_key);

void GPUMapEdges(const IdType *const global_src, IdType *const new_global_src,
                 const IdType *const global_dst, IdType *const new_global_dst,
                 const size_t num_edges, DeviceOrderedHashTable mapping,
                 Context ctx, StreamHandle stream);

void GPUExtract(void *dst, const void *src, const IdType *index,
                size_t num_index, size_t dim, DataType dtype, Context ctx,
                StreamHandle stream, uint64_t task_key);

void GPUBatchSanityCheck(IdType *map, const IdType *input,
                         const size_t num_input, Context ctx,
                         StreamHandle stream);

}  // namespace cuda
}  // namespace common
}  // namespace samgraph

#endif  // SAMGRAPH_CUDA_FUNCTION_H
