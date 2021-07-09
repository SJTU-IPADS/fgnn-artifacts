#ifndef SAMGRAPH_COMMON_H
#define SAMGRAPH_COMMON_H

#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace samgraph {
namespace common {

using IdType = unsigned int;
using Id64Type = unsigned long long int;

enum DataType {
  kF32 = 0,
  kF64 = 1,
  kF16 = 2,
  kU8 = 3,
  kI32 = 4,
  kI8 = 5,
  kI64 = 6,
};

enum DeviceType { kCPU = 0, kMMAP = 1, kGPU = 2 };

enum SampleType {
  kKHop0 = 0,  // vertex-parallel
  kKHop1,      // sample-parallel
  kWeightedKHop,
  kRandomWalk
};

// arch0: vanilla mode(CPU sampling + GPU training)
// arch1: standalone mode (single GPU for both sampling and training)
// arch2: offload mode (offload the feature extraction to CPU)
// arch3: dedicated mode (dedicated GPU for sampling and training)
enum RunArch { kArch0 = 0, kArch1, kArch2, kArch3 };

// cache by degree: cache the nodes with large degree
// cache by heuristic: cache the training set and the first hop neighbors first,
// then the nodes with large degree
enum CachePolicy { kCacheByDegree = 0, kCacheByHeuristic };

struct Context {
  DeviceType device_type;
  int device_id;
};

using StreamHandle = void*;

class Tensor;
using TensorPtr = std::shared_ptr<Tensor>;

class Tensor {
 public:
  Tensor();
  ~Tensor();

  bool Defined() const { return _data; }
  DataType Type() const { return _dtype; }
  const std::vector<size_t>& Shape() const { return _shape; }
  const void* Data() const { return _data; }
  void* MutableData() { return _data; }
  size_t NumBytes() const { return _nbytes; }
  Context Ctx() const { return _ctx; }

  static TensorPtr Null();
  static TensorPtr Empty(DataType dtype, std::vector<size_t> shape, Context ctx,
                         std::string name);
  static TensorPtr Copy1D(TensorPtr tensor, size_t item_offset,
                          std::vector<size_t> shape, std::string name,
                          StreamHandle stream = nullptr);
  static TensorPtr FromMmap(std::string filepath, DataType dtype,
                            std::vector<size_t> shape, Context ctx,
                            std::string name, StreamHandle stream = nullptr);
  static TensorPtr FromBlob(void* data, DataType dtype,
                            std::vector<size_t> shape, Context ctx,
                            std::string name);

 private:
  void* _data;
  DataType _dtype;
  Context _ctx;

  size_t _nbytes;
  std::vector<size_t> _shape;

  std::string _name;
};

// Graph dataset that should be loaded from the disk using MMAP.
struct Dataset {
  // Graph topology data
  TensorPtr indptr;
  TensorPtr indices;
  size_t num_node;
  size_t num_edge;

  TensorPtr prob_table;
  TensorPtr alias_table;

  TensorPtr in_degrees;
  TensorPtr out_degrees;

  // Decide nodes' feature store in GPU or CPU
  TensorPtr ranking_nodes;

  // Node feature and label
  size_t num_class;
  TensorPtr feat;
  TensorPtr label;

  // Node set
  TensorPtr train_set;
  TensorPtr test_set;
  TensorPtr valid_set;
};

// Train graph in COO format
struct TrainGraph {
  TensorPtr row;
  TensorPtr col;
  TensorPtr data;
  size_t num_src;
  size_t num_dst;
  size_t num_edge;
};

struct Task {
  // Key of the task
  uint64_t key;
  // Output graph tensor
  std::vector<std::shared_ptr<TrainGraph>> graphs;
  // node ids of the last train graph
  TensorPtr input_nodes;
  // node ids of the first train graph
  TensorPtr output_nodes;
  // Input feature tensor
  TensorPtr input_feat;
  // Output label tensor
  TensorPtr output_label;
};

using GraphBatch = Task;
using TaskPtr = std::shared_ptr<Task>;

typedef void (*LoopFunction)();
typedef bool (*LoopOnceFunction)();

Context CPU(int device_id = 0);
Context GPU(int device_id = 0);
Context MMAP(int device_id = 0);

size_t GetDataTypeBytes(DataType dtype);
size_t GetTensorBytes(DataType dtype, const std::vector<size_t> shape);
size_t GetTensorBytes(DataType dtype,
                      std::vector<size_t>::const_iterator shape_start,
                      std::vector<size_t>::const_iterator shape_end);
// predict the number of sampled nodes
size_t PredictNumNodes(size_t batch_size, const std::vector<size_t>& fanout,
                       size_t num_fanout_to_comp);
size_t PredictNumRandomWalkEdges(size_t batch_size,
                                 const std::vector<size_t>& fanout,
                                 size_t num_fanout_to_comp,
                                 size_t num_random_walk,
                                 size_t random_walk_length);

std::string ToReadableSize(size_t nbytes);
std::string ToPercentage(double percentage);

std::string GetEnv(std::string key);
bool IsEnvSet(std::string key);
std::string GetTimeString();
bool FileExist(const std::string& filepath);

template <typename T>
inline T RoundUpDiv(T target, T unit) {
  return (target + unit - 1) / unit;
}

template <typename T>
inline T RoundUp(T target, T unit) {
  return RoundUpDiv(target, unit) * unit;
}

template <typename T>
inline T Max(T a, T b) {
  return a > b ? a : b;
}

template <typename T>
inline T Min(T a, T b) {
  return a < b ? a : b;
}

}  // namespace common
}  // namespace samgraph

#endif  // SAMGRAPH_COMMON_H
