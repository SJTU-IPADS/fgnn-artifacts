#include <atomic>
#include <cuda_runtime.h>

#include <chrono>
#include <numeric>
#include <thread>

#include "../device.h"
#include "../function.h"
#include "../logging.h"
#include "../profiler.h"
#include "../run_config.h"
#include "../timer.h"
#include "cuda_common.h"
#include "cuda_engine.h"
#include "cuda_function.h"
#include "cuda_hashtable.h"
#include "cuda_loops.h"

/* clang-format off
 * +-----------------------+     +--------------------+     +------------------------+
 * |                       |     |                    |     |                        |
 * |                       |     |                    |     |                        |
 * |                       |     |                    |     |                        |
 * |       Sampling        ------> Feature Extraction ------>        Training        |
 * |                       |     |                    |     |                        |
 * |                       |     |                    |     |                        |
 * | Dedicated Sampler GPU |     |         CPU        |     | Dedicated Trainer GPU  |
 * +-----------------------+     +--------------------+     +------------------------+
 * clang-format on
 */

namespace samgraph {
namespace common {
namespace cuda {

namespace {

bool RunSampleSubLoopOnce() {
  auto next_op = kDataCopy;
  auto next_q = GPUEngine::Get()->GetTaskQueue(next_op);
  if (next_q->Full()) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    return true;
  }

  Timer t0;
  auto task = DoShuffle();
  if (task) {
    double shuffle_time = t0.Passed();

    std::function <void(TaskPtr)> nbr_cb = [next_q](TaskPtr tp) {
      next_q->AddTask(tp);
    };
    Timer t1;
    DoGPUSampleDyCache(task, nbr_cb);
    double sample_time = t1.Passed();

    Profiler::Get().LogStep(task->key, kLogL1SampleTime,
                            shuffle_time + sample_time);
    Profiler::Get().LogStep(task->key, kLogL2ShuffleTime, shuffle_time);
    Profiler::Get().LogEpochAdd(task->key, kLogEpochSampleTime,
                                shuffle_time + sample_time);
  } else {
    std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
  }

  return true;
}

bool RunDataCopySubLoopOnce() {
  auto graph_pool = GPUEngine::Get()->GetGraphPool();
  if (graph_pool->Full()) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    return true;
  }

  auto this_op = kDataCopy;
  auto q = GPUEngine::Get()->GetTaskQueue(this_op);
  auto task = q->GetTask();

  if (task) {
    Timer t2;
    DoIdCopy(task);
    double id_copy_time = t2.Passed();

    Timer t0;
    DoCPUFeatureExtract(task);
    double extract_time = t0.Passed();

    Timer t1;
    DoFeatureCopy(task);
    double feat_copy_time = t1.Passed();


    LOG(DEBUG) << "Waiting for edge remapping " << task->key;
    while(task->graph_remapped.load(std::memory_order_acquire) == false) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    }

    Timer t3;
    DoGraphCopy(task);
    double graph_copy_time = t3.Passed();

    LOG(DEBUG) << "Submit: process task with key " << task->key;
    graph_pool->Submit(task->key, task);

    Profiler::Get().LogStep(
        task->key, kLogL1CopyTime,
        graph_copy_time + id_copy_time + extract_time + feat_copy_time);
    Profiler::Get().LogStep(task->key, kLogL2GraphCopyTime, graph_copy_time);
    Profiler::Get().LogStep(task->key, kLogL2IdCopyTime, id_copy_time);
    Profiler::Get().LogStep(task->key, kLogL2ExtractTime, extract_time);
    Profiler::Get().LogStep(task->key, kLogL2FeatCopyTime, feat_copy_time);
    Profiler::Get().LogEpochAdd(
        task->key, kLogEpochCopyTime,
        graph_copy_time + id_copy_time + extract_time + feat_copy_time);
  } else {
    std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
  }

  return true;
}

void SampleSubLoop() {
  while (RunSampleSubLoopOnce() && !GPUEngine::Get()->ShouldShutdown()) {
  }
  GPUEngine::Get()->ReportThreadFinish();
}

void DataCopySubLoop() {
  LoopOnceFunction func = RunDataCopySubLoopOnce;

  while (func() && !GPUEngine::Get()->ShouldShutdown()) {
  }

  GPUEngine::Get()->ReportThreadFinish();
}

}  // namespace

void RunArch4LoopsOnce() {
  RunSampleSubLoopOnce();
  RunDataCopySubLoopOnce();
}

std::vector<LoopFunction> GetArch4Loops() {
  std::vector<LoopFunction> func;

  func.push_back(SampleSubLoop);
  func.push_back(DataCopySubLoop);

  return func;
}

}  // namespace cuda
}  // namespace common
}  // namespace samgraph
