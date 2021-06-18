#include "operation.h"

#include <cuda_profiler_api.h>
#include <cuda_runtime.h>

#include <string>
#include <vector>

#include "common.h"
#include "constant.h"
#include "engine.h"
#include "logging.h"
#include "profiler.h"
#include "run_config.h"

namespace samgraph {
namespace common {

extern "C" {

void samgraph_config(const char *path, int run_arch, int sample_type,
                     int sampler_device_type, int sampler_device_id,
                     int trainer_device_type, int trainer_device_id,
                     size_t batch_size, int *fanout, size_t num_fanout,
                     size_t num_epoch, double cache_percentage) {
  RunConfig::dataset_path = path;
  RunConfig::run_arch = static_cast<RunArch>(run_arch);
  RunConfig::sample_type = static_cast<SampleType>(sample_type);
  RunConfig::fanout = std::vector<int>(fanout, fanout + num_fanout);
  RunConfig::batch_size = batch_size;
  RunConfig::num_epoch = num_epoch;
  RunConfig::sampler_ctx =
      Context{static_cast<DeviceType>(sampler_device_type), sampler_device_id};
  RunConfig::trainer_ctx =
      Context{static_cast<DeviceType>(trainer_device_type), trainer_device_id};
  RunConfig::cache_percentage = cache_percentage;

  RunConfig::LoadConfigFromEnv();
}

void samgraph_init() {
  Engine::Create();
  Engine::Get()->Init();

  LOG(INFO) << "SamGraph has been initialied successfully";
}

void samgraph_start() {
  CHECK(Engine::Get()->IsInitialized() && !Engine::Get()->IsShutdown());
  if (RunConfig::option_profile_cuda) {
    CUDA_CALL(cudaProfilerStart());
  }

  Engine::Get()->Start();
  LOG(INFO) << "SamGraph has been started successfully";
}

size_t samgraph_num_epoch() {
  CHECK(Engine::Get()->IsInitialized() && !Engine::Get()->IsShutdown());
  return Engine::Get()->NumEpoch();
}

size_t samgraph_steps_per_epoch() {
  CHECK(Engine::Get()->IsInitialized() && !Engine::Get()->IsShutdown());
  return Engine::Get()->NumStep();
}

size_t samgraph_num_class() {
  CHECK(Engine::Get()->IsInitialized() && !Engine::Get()->IsShutdown());
  return Engine::Get()->GetGraphDataset()->num_class;
}

size_t samgraph_feat_dim() {
  CHECK(Engine::Get()->IsInitialized() && !Engine::Get()->IsShutdown());
  return Engine::Get()->GetGraphDataset()->feat->Shape().at(1);
}

uint64_t samgraph_get_next_batch(uint64_t epoch, uint64_t step) {
  CHECK(Engine::Get()->IsInitialized() && !Engine::Get()->IsShutdown());

  uint64_t key = Engine::Get()->GetBatchKey(epoch, step);
  LOG(DEBUG) << "samgraph_get_next_batch encodeKey with epoch " << epoch
             << " step " << step << " and key " << key;
  auto graph = Engine::Get()->GetGraphPool()->GetGraphBatch(key);

  LOG(DEBUG) << "Get next batch with key " << key;
  Engine::Get()->SetGraphBatch(graph);

  return key;
}

void samgraph_sample_once() { Engine::Get()->RunSampleOnce(); }

size_t samgraph_get_graph_num_row(uint64_t key, int graph_id) {
  auto batch = Engine::Get()->GetGraphBatch();
  return batch->graphs[graph_id]->num_row;
}

size_t samgraph_get_graph_num_col(uint64_t key, int graph_id) {
  auto batch = Engine::Get()->GetGraphBatch();
  return batch->graphs[graph_id]->num_column;
}

size_t samgraph_get_graph_num_edge(uint64_t key, int graph_id) {
  auto batch = Engine::Get()->GetGraphBatch();
  return batch->graphs[graph_id]->num_edge;
}

void samgraph_shutdown() {
  Engine::Get()->Shutdown();
  if (RunConfig::option_profile_cuda) {
    CUDA_CALL(cudaProfilerStop());
  }
  LOG(INFO) << "SamGraph has been completely shutdown now";
}

void samgraph_report(uint64_t epoch, uint64_t step) {
  Engine::Get()->Report(epoch, step);
}

void samgraph_report_node_access() {
  if (RunConfig::option_log_node_access) {
    Profiler::Get().ReportNodeAccess();
  }
}
}
}  // namespace common
}  // namespace samgraph
