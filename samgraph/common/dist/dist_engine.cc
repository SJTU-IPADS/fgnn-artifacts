#include "dist_engine.h"

#include <semaphore.h>
#include <stddef.h>
#include <chrono>
#include <cstdlib>
#include <numeric>

#include "../constant.h"
#include "../device.h"
#include "../logging.h"
#include "../run_config.h"
#include "../timer.h"
#include "../cuda/cuda_common.h"
#include "../cpu/cpu_engine.h"
#include "dist_loops.h"
#include "pre_sampler.h"

// XXX: decide CPU or GPU to shuffling, sampling and id remapping
/*
#include "cpu_hashtable0.h"
#include "cpu_hashtable1.h"
#include "cpu_hashtable2.h"
#include "cpu_loops.h"
*/

namespace samgraph {
namespace common {
namespace dist {

DistEngine::DistEngine() {
  _initialize = false;
  _should_shutdown = false;
}

void DistEngine::Init() {
  if (_initialize) {
    return;
  }

  _dataset_path = RunConfig::dataset_path;
  _batch_size = RunConfig::batch_size;
  _fanout = RunConfig::fanout;
  _num_epoch = RunConfig::num_epoch;
  _joined_thread_cnt = 0;
  _sample_stream = nullptr;
  _sampler_copy_stream = nullptr;
  _trainer_copy_stream = nullptr;
  _dist_type = DistType::Default;
  _shuffler = nullptr;
  _random_states = nullptr;
  _cache_manager = nullptr;
  _frequency_hashmap = nullptr;
  _cache_hashtable = nullptr;

  // Check whether the ctx configuration is allowable
  DistEngine::ArchCheck();

  // Load the target graph data
  LoadGraphDataset();

  if (RunConfig::UseGPUCache()) {
    switch (RunConfig::cache_policy) {
      case kCacheByPreSampleStatic:
      case kCacheByPreSample: {
        size_t nbytes = sizeof(IdType) * _dataset->num_node;
        void *shared_ptr = (mmap(NULL, nbytes, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0));
        _dataset->ranking_nodes = Tensor::FromBlob(
            shared_ptr, DataType::kI32, {_dataset->num_node}, Context{kMMAP, 0}, "ranking_nodes");
        break;
      }
      default: ;
    }
  }

  LOG(DEBUG) << "Finished pre-initialization";
}

void DistEngine::SampleDataCopy(Context sampler_ctx, StreamHandle stream) {
  _dataset->train_set = Tensor::CopyTo(_dataset->train_set, CPU(), stream);
  _dataset->valid_set = Tensor::CopyTo(_dataset->valid_set, CPU(), stream);
  _dataset->test_set = Tensor::CopyTo(_dataset->test_set, CPU(), stream);
  if (sampler_ctx.device_type == kGPU) {
    _dataset->indptr = Tensor::CopyTo(_dataset->indptr, sampler_ctx, stream);
    _dataset->indices = Tensor::CopyTo(_dataset->indices, sampler_ctx, stream);
    if (RunConfig::sample_type == kWeightedKHop) {
      _dataset->prob_table->Tensor::CopyTo(_dataset->prob_table, sampler_ctx, stream);
      _dataset->alias_table->Tensor::CopyTo(_dataset->prob_table, sampler_ctx, stream);
    }
  }
  LOG(DEBUG) << "SampleDataCopy finished!";
}

void DistEngine::SampleCacheTableInit() {
  size_t num_nodes = _dataset->num_node;
  auto nodes = static_cast<const IdType*>(_dataset->ranking_nodes->Data());
  size_t num_cached_nodes = num_nodes *
                            (RunConfig::cache_percentage);
  auto cpu_device = Device::Get(CPU());
  auto sampler_gpu_device = Device::Get(_sampler_ctx);

  IdType *tmp_cpu_hashtable = static_cast<IdType *>(
      cpu_device->AllocDataSpace(CPU(), sizeof(IdType) * num_nodes));
  _cache_hashtable =
      static_cast<IdType *>(sampler_gpu_device->AllocDataSpace(
          _sampler_ctx, sizeof(IdType) * num_nodes));

  // 1. Initialize the cpu hashtable
#pragma omp parallel for num_threads(RunConfig::kOMPThreadNum)
  for (size_t i = 0; i < num_nodes; i++) {
    tmp_cpu_hashtable[i] = Constant::kEmptyKey;
  }

  // 2. Populate the cpu hashtable
#pragma omp parallel for num_threads(RunConfig::kOMPThreadNum)
  for (size_t i = 0; i < num_cached_nodes; i++) {
    tmp_cpu_hashtable[nodes[i]] = i;
  }

  // 3. Copy the cache from the cpu memory to gpu memory
  sampler_gpu_device->CopyDataFromTo(
      tmp_cpu_hashtable, 0, _cache_hashtable, 0,
      sizeof(IdType) * num_nodes, CPU(), _sampler_ctx);

  // 4. Free the cpu tmp cache data
  cpu_device->FreeDataSpace(CPU(), tmp_cpu_hashtable);

  std::unordered_map<CachePolicy, std::string> policy2str = {
      {kCacheByDegree, "degree"},
      {kCacheByHeuristic, "heuristic"},
      {kCacheByPreSample, "preSample"},
      {kCacheByPreSampleStatic, "preSampleStatic"},
      {kCacheByDegreeHop, "degree_hop"},
      {kCacheByFakeOptimal, "fake_optimal"},
  };

  LOG(INFO) << "GPU cache (policy: " << policy2str.at(RunConfig::cache_policy)
            << ") " << num_cached_nodes << " / " << num_nodes;
}

void DistEngine::SampleInit(int device_type, int device_id,
    int sampler_id, int num_sampler, int num_trainer) {
  if (_initialize) {
    LOG(FATAL) << "DistEngine already initialized!";
    return;
  }
  _dist_type = DistType::Sample;
  RunConfig::sampler_ctx = Context{static_cast<DeviceType>(device_type), device_id};
  _sampler_ctx = RunConfig::sampler_ctx;
  if (_sampler_ctx.device_type == kGPU) {
    _sample_stream = Device::Get(_sampler_ctx)->CreateStream(_sampler_ctx);
    // use sampler_ctx in task sending
    _sampler_copy_stream = Device::Get(_sampler_ctx)->CreateStream(_sampler_ctx);

    Device::Get(_sampler_ctx)->StreamSync(_sampler_ctx, _sample_stream);
    Device::Get(_sampler_ctx)->StreamSync(_sampler_ctx, _sampler_copy_stream);
  }

  SampleDataCopy(_sampler_ctx, _sample_stream);

  _shuffler = nullptr;
  switch(device_type) {
    case kCPU:
      _shuffler = new CPUShuffler(_dataset->train_set,
          _num_epoch, _batch_size, false);
      break;
    case kGPU:
      _shuffler = new DistShuffler(_dataset->train_set,
          _num_epoch, _batch_size, sampler_id, num_sampler, num_trainer, false);
      break;
    default:
        LOG(FATAL) << "shuffler does not support device_type: "
                   << device_type;
  }
  _num_step = _shuffler->NumStep();


  // XXX: map the _hash_table to difference device
  //       _hashtable only support GPU device
#ifndef SXN_NAIVE_HASHMAP
  _hashtable = new cuda::OrderedHashTable(
      PredictNumNodes(_batch_size, _fanout, _fanout.size()), _sampler_ctx);
#else
  _hashtable = new cuda::OrderedHashTable(
      _dataset->num_node, _sampler_ctx, 1);
#endif

  // Create CUDA random states for sampling
  _random_states = new cuda::GPURandomStates(RunConfig::sample_type, _fanout,
                                       _batch_size, _sampler_ctx);

  if (RunConfig::sample_type == kRandomWalk) {
    size_t max_nodes =
        PredictNumNodes(_batch_size, _fanout, _fanout.size() - 1);
    size_t edges_per_node =
        RunConfig::num_random_walk * RunConfig::random_walk_length;
    _frequency_hashmap =
        new cuda::FrequencyHashmap(max_nodes, edges_per_node, _sampler_ctx);
  } else {
    _frequency_hashmap = nullptr;
  }

  // Create queues
  for (int i = 0; i < cuda::QueueNum; i++) {
    LOG(DEBUG) << "Create task queue" << i;
    if (static_cast<cuda::QueueType>(i) == cuda::kDataCopy) {
      _queues.push_back(new MessageTaskQueue(RunConfig::max_copying_jobs));
    }
    else {
      _queues.push_back(new TaskQueue(RunConfig::max_sampling_jobs));
    }
  }
  // batch results set
  _graph_pool = new GraphPool(RunConfig::max_copying_jobs);

  if (RunConfig::UseGPUCache()) {
    switch (RunConfig::cache_policy) {
      case kCacheByPreSampleStatic:
      case kCacheByPreSample: {
        PreSampler::SetSingleton(new PreSampler(_dataset->num_node, NumStep()));
        auto rank_results = static_cast<const IdType*>(PreSampler::Get()->DoPreSample()->Data());
        auto rank_node = static_cast<IdType*>(_dataset->ranking_nodes->MutableData());
        size_t num_node = _dataset->num_node;
#pragma omp parallel for num_threads(RunConfig::kOMPThreadNum)
        for (size_t i = 0; i < num_node; ++i) {
          rank_node[i] = rank_results[i];
        }
        break;
      }
      default: ;
    }
    SampleCacheTableInit();
  }

  _initialize = true;
}

void DistEngine::TrainDataCopy(Context trainer_ctx, StreamHandle stream) {
  _dataset->label = Tensor::CopyTo(_dataset->label, trainer_ctx, stream);
  LOG(DEBUG) << "TrainDataCopy finished!";
}

void DistEngine::TrainInit(int device_type, int device_id) {
  if (_initialize) {
    LOG(FATAL) << "DistEngine already initialized!";
    return;
  }
  _dist_type = DistType::Extract;
  RunConfig::trainer_ctx = Context{static_cast<DeviceType>(device_type), device_id};
  _trainer_ctx = RunConfig::trainer_ctx;

  // Create CUDA streams
  // XXX: create cuda streams that training needs
  //       only support GPU sampling
  _trainer_copy_stream = Device::Get(_trainer_ctx)->CreateStream(_trainer_ctx);
  Device::Get(_trainer_ctx)->StreamSync(_trainer_ctx, _trainer_copy_stream);
  // next code is for CPU sampling
  /*
  _work_stream = static_cast<cudaStream_t>(
      Device::Get(_trainer_ctx)->CreateStream(_trainer_ctx));
  Device::Get(_trainer_ctx)->StreamSync(_trainer_ctx, _work_stream);
  */

  _num_step = ((_dataset->train_set->Shape().front() + _batch_size - 1) / _batch_size);

  if (RunConfig::UseGPUCache()) {
    TrainDataCopy(_trainer_ctx, _trainer_copy_stream);
    // wait the presample
    // XXX: let the app ensure sampler initialization before trainer
    /*
    switch (RunConfig::cache_policy) {
      case kCacheByPreSampleStatic:
      case kCacheByPreSample: {
        break;
      }
      default: ;
    }
    */
    _cache_manager = new DistCacheManager(
        _trainer_ctx, _dataset->feat->Data(),
        _dataset->feat->Type(), _dataset->feat->Shape()[1],
        static_cast<const IdType*>(_dataset->ranking_nodes->Data()),
        _dataset->num_node, RunConfig::cache_percentage);
  } else {
    _cache_manager = nullptr;
  }

  // Create queues
  for (int i = 0; i < cuda::QueueNum; i++) {
    LOG(DEBUG) << "Create task queue" << i;
    if (static_cast<cuda::QueueType>(i) == cuda::kDataCopy) {
      _queues.push_back(new MessageTaskQueue(RunConfig::max_copying_jobs));
    }
    else {
      _queues.push_back(new TaskQueue(RunConfig::max_sampling_jobs));
    }
  }
  // results pool
  _graph_pool = new GraphPool(RunConfig::max_copying_jobs);

  _initialize = true;
}


void DistEngine::Start() {
  LOG(FATAL) << "DistEngine needs not implement the Start function!!!";
}

/**
 * @param count: the total times to loop
 */
void DistEngine::StartExtract(int count) {
  ExtractFunction func;
  switch (RunConfig::run_arch) {
    case kArch5:
      func = GetArch5Loops();
      break;
    default:
      // Not supported arch 0
      CHECK(0);
  }

  // Start background threads
  _threads.push_back(new std::thread(func, count));
  LOG(DEBUG) << "Started a extracting background threads.";
}

void DistEngine::Shutdown() {
  if (_should_shutdown) {
    return;
  }

  _should_shutdown = true;
  int total_thread_num = _threads.size();

  while (!IsAllThreadFinish(total_thread_num)) {
    // wait until all threads joined
    std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
  }

  for (size_t i = 0; i < _threads.size(); i++) {
    _threads[i]->join();
    delete _threads[i];
    _threads[i] = nullptr;
  }

  // free queue
  for (size_t i = 0; i < cuda::QueueNum; i++) {
    if (_queues[i]) {
      delete _queues[i];
      _queues[i] = nullptr;
    }
  }

  if (_dist_type == DistType::Sample) {
    Device::Get(_sampler_ctx)->StreamSync(_sampler_ctx, _sample_stream);
    Device::Get(_sampler_ctx)->FreeStream(_sampler_ctx, _sample_stream);
    Device::Get(_sampler_ctx)->StreamSync(_sampler_ctx, _sampler_copy_stream);
    Device::Get(_sampler_ctx)->FreeStream(_sampler_ctx, _sampler_copy_stream);
  }
  else if (_dist_type == DistType::Extract) {
    Device::Get(_trainer_ctx)->StreamSync(_trainer_ctx, _trainer_copy_stream);
    Device::Get(_trainer_ctx)->FreeStream(_trainer_ctx, _trainer_copy_stream);
  }
  else {
    LOG(FATAL) << "_dist_type is illegal!";
  }

  delete _dataset;
  delete _graph_pool;
  if (_shuffler != nullptr) {
    delete _shuffler;
  }
  if (_random_states != nullptr) {
    delete _random_states;
  }

  if (_cache_manager != nullptr) {
    delete _cache_manager;
  }

  if (_frequency_hashmap != nullptr) {
    delete _frequency_hashmap;
  }

  if (_cache_hashtable != nullptr) {
    Device::Get(_sampler_ctx)->FreeDataSpace(_sampler_ctx, _cache_hashtable);
  }

  _dataset = nullptr;
  _shuffler = nullptr;
  _graph_pool = nullptr;
  _cache_manager = nullptr;
  _random_states = nullptr;
  _frequency_hashmap = nullptr;

  _threads.clear();
  _joined_thread_cnt = 0;
  _initialize = false;
  _should_shutdown = false;
  LOG(INFO) << "DistEngine shutdown successfully!";
}

void DistEngine::RunSampleOnce() {
  switch (RunConfig::run_arch) {
    case kArch5:
      RunArch5LoopsOnce(_dist_type);
      break;
    default:
      CHECK(0);
  }
  LOG(DEBUG) << "RunSampleOnce finished.";
}

void DistEngine::ArchCheck() {
  CHECK_EQ(RunConfig::run_arch, kArch5);
  CHECK(!(RunConfig::UseGPUCache() && RunConfig::option_log_node_access));
}

std::unordered_map<std::string, Context> DistEngine::GetGraphFileCtx() {
  std::unordered_map<std::string, Context> ret;

  ret[Constant::kIndptrFile] = MMAP();
  ret[Constant::kIndicesFile] = MMAP();
  ret[Constant::kFeatFile] = MMAP();
  ret[Constant::kLabelFile] = MMAP();
  ret[Constant::kTrainSetFile] = MMAP();
  ret[Constant::kTestSetFile] = MMAP();
  ret[Constant::kValidSetFile] = MMAP();
  ret[Constant::kProbTableFile] = MMAP();
  ret[Constant::kAliasTableFile] = MMAP();
  ret[Constant::kInDegreeFile] = MMAP();
  ret[Constant::kOutDegreeFile] = MMAP();
  ret[Constant::kCacheByDegreeFile] = MMAP();
  ret[Constant::kCacheByHeuristicFile] = MMAP();
  ret[Constant::kCacheByDegreeHopFile] = MMAP();
  ret[Constant::kCacheByFakeOptimalFile] = MMAP();

  return ret;
}

}  // namespace dist
}  // namespace common
}  // namespace samgraph
