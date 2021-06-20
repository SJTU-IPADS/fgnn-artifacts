#include "engine.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <unordered_map>

#include "common.h"
#include "constant.h"
#include "cpu/cpu_engine.h"
#include "cuda/cuda_engine.h"
#include "logging.h"
#include "profiler.h"
#include "run_config.h"
#include "timer.h"

namespace samgraph {
namespace common {

Engine* Engine::_engine = nullptr;

void Engine::Report(uint64_t epoch, uint64_t step) {
  uint64_t key = Engine::GetBatchKey(epoch, step);
  if (RunConfig::option_report_step) {
    Profiler::Get().Report(key);
  } else {
    Profiler::Get().ReportAverage(key);
  }
}

void Engine::Create() {
  if (_engine) {
    return;
  }

  switch (RunConfig::run_arch) {
    case kArch0:
      LOG(INFO) << "Use CPU Engine";
      _engine = new cpu::CPUEngine();
      break;
    case kArch1:
    case kArch2:
    case kArch3:
      LOG(INFO) << "Use GPU Engine";
      _engine = new cuda::GPUEngine();
      break;
    default:
      CHECK(0);
  }
}

void Engine::LoadGraphDataset() {
  Timer t;
  // Load graph dataset from disk by mmap and copy the graph
  // topology data into the target CUDA device.
  _dataset = new Dataset;
  std::unordered_map<std::string, size_t> meta;
  std::unordered_map<std::string, Context> ctx_map = GetGraphFileCtx();

  if (_dataset_path.back() != '/') {
    _dataset_path.push_back('/');
  }

  // Parse the meta data
  std::ifstream meta_file(_dataset_path + Constant::kMetaFile);
  std::string line;
  while (std::getline(meta_file, line)) {
    std::istringstream iss(line);
    std::vector<std::string> kv{std::istream_iterator<std::string>{iss},
                                std::istream_iterator<std::string>{}};

    if (kv.size() < 2) {
      break;
    }

    meta[kv[0]] = std::stoull(kv[1]);
  }

  CHECK(meta.count(Constant::kMetaNumNode) > 0);
  CHECK(meta.count(Constant::kMetaNumEdge) > 0);
  CHECK(meta.count(Constant::kMetaFeatDim) > 0);
  CHECK(meta.count(Constant::kMetaNumClass) > 0);
  CHECK(meta.count(Constant::kMetaNumTrainSet) > 0);
  CHECK(meta.count(Constant::kMetaNumTestSet) > 0);
  CHECK(meta.count(Constant::kMetaNumValidSet) > 0);

  CHECK(ctx_map.count(Constant::kIndptrFile) > 0);
  CHECK(ctx_map.count(Constant::kIndicesFile) > 0);
  CHECK(ctx_map.count(Constant::kFeatFile) > 0);
  CHECK(ctx_map.count(Constant::kLabelFile) > 0);
  CHECK(ctx_map.count(Constant::kTrainSetFile) > 0);
  CHECK(ctx_map.count(Constant::kTestSetFile) > 0);
  CHECK(ctx_map.count(Constant::kValidSetFile) > 0);
  CHECK(ctx_map.count(Constant::kAliasTableFile) > 0);
  CHECK(ctx_map.count(Constant::kProbTableFile) > 0);
  CHECK(ctx_map.count(Constant::kInDegreeFile) > 0);
  CHECK(ctx_map.count(Constant::kOutDegreeFile) > 0);
  CHECK(ctx_map.count(Constant::kSortedNodeByInDegreeFile) > 0);

  _dataset->num_node = meta[Constant::kMetaNumNode];
  _dataset->num_edge = meta[Constant::kMetaNumEdge];
  _dataset->num_class = meta[Constant::kMetaNumClass];

  _dataset->indptr =
      Tensor::FromMmap(_dataset_path + Constant::kIndptrFile, DataType::kI32,
                       {meta[Constant::kMetaNumNode] + 1},
                       ctx_map[Constant::kIndptrFile], "dataset.indptr");
  _dataset->indices =
      Tensor::FromMmap(_dataset_path + Constant::kIndicesFile, DataType::kI32,
                       {meta[Constant::kMetaNumEdge]},
                       ctx_map[Constant::kIndicesFile], "dataset.indices");

  if (FileExist(_dataset_path + Constant::kFeatFile)) {
    _dataset->feat = Tensor::FromMmap(
        _dataset_path + Constant::kFeatFile, DataType::kF32,
        {meta[Constant::kMetaNumNode], meta[Constant::kMetaFeatDim]},
        ctx_map[Constant::kFeatFile], "dataset.feat");
  } else {
    _dataset->feat = Tensor::Empty(
        DataType::kF32,
        {meta[Constant::kMetaNumNode], meta[Constant::kMetaFeatDim]},
        ctx_map[Constant::kFeatFile], "dataset.feat");
  }

  if (FileExist(_dataset_path + Constant::kLabelFile)) {
    _dataset->label =
        Tensor::FromMmap(_dataset_path + Constant::kLabelFile, DataType::kI64,
                         {meta[Constant::kMetaNumNode]},
                         ctx_map[Constant::kLabelFile], "dataset.label");
  } else {
    _dataset->label =
        Tensor::Empty(DataType::kI64, {meta[Constant::kMetaNumNode]},
                      ctx_map[Constant::kLabelFile], "dataset.label");
  }

  _dataset->train_set =
      Tensor::FromMmap(_dataset_path + Constant::kTrainSetFile, DataType::kI32,
                       {meta[Constant::kMetaNumTrainSet]},
                       ctx_map[Constant::kTrainSetFile], "dataset.train_set");
  _dataset->test_set =
      Tensor::FromMmap(_dataset_path + Constant::kTestSetFile, DataType::kI32,
                       {meta[Constant::kMetaNumTestSet]},
                       ctx_map[Constant::kTestSetFile], "dataset.test_set");
  _dataset->valid_set =
      Tensor::FromMmap(_dataset_path + Constant::kValidSetFile, DataType::kI32,
                       {meta[Constant::kMetaNumValidSet]},
                       ctx_map[Constant::kValidSetFile], "dataset.valid_set");

  if (RunConfig::sample_type == kWeightedKHop) {
    _dataset->prob_table = Tensor::FromMmap(
        _dataset_path + Constant::kProbTableFile, DataType::kF32,
        {meta[Constant::kMetaNumEdge]}, ctx_map[Constant::kProbTableFile],
        "dataset.prob_table");

    _dataset->alias_table = Tensor::FromMmap(
        _dataset_path + Constant::kAliasTableFile, DataType::kI32,
        {meta[Constant::kMetaNumEdge]}, ctx_map[Constant::kAliasTableFile],
        "dataset.alias_table");
  } else {
    _dataset->prob_table = Tensor::Null();
    _dataset->alias_table = Tensor::Null();
  }

  _dataset->in_degrees =
      Tensor::FromMmap(_dataset_path + Constant::kInDegreeFile, DataType::kI32,
                       {meta[Constant::kMetaNumNode]},
                       ctx_map[Constant::kInDegreeFile], "dataset.in_degrees");
  _dataset->out_degrees = Tensor::FromMmap(
      _dataset_path + Constant::kOutDegreeFile, DataType::kI32,
      {meta[Constant::kMetaNumNode]}, ctx_map[Constant::kOutDegreeFile],
      "dataset.out_degrees");
  _dataset->sorted_nodes_by_in_degree =
      Tensor::FromMmap(_dataset_path + Constant::kSortedNodeByInDegreeFile,
                       DataType::kI32, {meta[Constant::kMetaNumNode]},
                       ctx_map[Constant::kSortedNodeByInDegreeFile],
                       "dataset.sorted_nodes_by_in_degree");

  double loading_time = t.Passed();
  LOG(INFO) << "SamGraph loaded dataset(" << _dataset_path << ") successfully ("
            << loading_time << " secs)";
}

bool Engine::IsAllThreadFinish(int total_thread_num) {
  int k = _joined_thread_cnt.fetch_add(0);
  return (k == total_thread_num);
};

}  // namespace common
}  // namespace samgraph
