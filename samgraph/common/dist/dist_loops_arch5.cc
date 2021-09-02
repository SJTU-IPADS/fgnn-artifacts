#include "../cuda/cuda_loops.h"

#include <chrono>
#include <numeric>

#include "../device.h"
#include "../function.h"
#include "../logging.h"
#include "../profiler.h"
#include "../run_config.h"
#include "../timer.h"

#include "dist_loops.h"
#include "../cuda/cuda_function.h"
#include "../cuda/cuda_hashtable.h"
#include "../cuda/cuda_loops.h"

/* clang-format off
 * +-----------------------+        +--------------------+     +------------------------+
 * |                       |        |                    |     |                        |
 * |       Sampling        --Queue--> Feature Extraction ------>        Training        |
 * |                       |     |  |                    |     |                        |
 * | Dedicated Sampler GPU |     |  |         CPU        |     | Dedicated Trainer GPU  |
 * +-----------------------+     |  +--------------------+     +------------------------+
 *                               |
 *                               |  +--------------------+     +------------------------+
 *                               |  |                    |     |                        |
 *                               \--> Feature Extraction ------>        Training        |
 *                                  |                    |     |                        |
 *                                  |         CPU        |     | Dedicated Trainer GPU  |
 *                                  +--------------------+     +------------------------+
 * clang-format on
 */

namespace samgraph {
namespace common {
namespace dist {

namespace {

bool RunSampleSubLoopOnce() {
  auto graph_pool = DistEngine::Get()->GetGraphPool();
  if (graph_pool->Full()) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    return true;
  }

  auto next_op = cuda::kDataCopy;
  auto next_q = DistEngine::Get()->GetTaskQueue(next_op);
  if (next_q->Full()) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    return true;
  }

  Timer t0;
  auto task = DoShuffle();
  if (task) {
    double shuffle_time = t0.Passed();

    Timer t1;
    DoGPUSample(task);
    double sample_time = t1.Passed();

    LOG(DEBUG) << "RunSampleOnce next_q Send task";
    Timer t2;
    next_q->Send(task);
    double send_time = t2.Passed();

    Profiler::Get().LogStep(task->key, kLogL1SampleTime,
                            shuffle_time + sample_time + send_time);
    Profiler::Get().LogStep(task->key, kLogL1SendTime,
                            send_time);
    Profiler::Get().LogStep(task->key, kLogL2ShuffleTime, shuffle_time);
    Profiler::Get().LogEpochAdd(task->key, kLogEpochSampleTime,
                                shuffle_time + sample_time + send_time);
  } else {
    std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
  }

  return true;
}

bool RunDataCopySubLoopOnce() {
  auto graph_pool = DistEngine::Get()->GetGraphPool();
  while (graph_pool->Full()) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    // return true;
  }

  auto this_op = cuda::kDataCopy;
  auto q = DistEngine::Get()->GetTaskQueue(this_op);
  Timer t4;
  auto task = q->Recv();
  double recv_time = t4.Passed();

  if (task) {
    Timer t0;
    DoGraphCopy(task);
    double graph_copy_time = t0.Passed();

    Timer t1;
    DoIdCopy(task);
    double id_copy_time = t1.Passed();

    Timer t2;
    DoCPUFeatureExtract(task);
    double extract_time = t2.Passed();

    Timer t3;
    DoFeatureCopy(task);
    double feat_copy_time = t3.Passed();

    LOG(DEBUG) << "Submit: process task with key " << task->key;
    graph_pool->Submit(task->key, task);

    Profiler::Get().LogStep(
        task->key, kLogL1CopyTime,
        recv_time + graph_copy_time + id_copy_time + extract_time + feat_copy_time);
    Profiler::Get().LogStep(task->key, kLogL1RecvTime, recv_time);
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

bool RunCacheDataCopySubLoopOnce() {
  auto graph_pool = DistEngine::Get()->GetGraphPool();
  while (graph_pool->Full()) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    // return true;
  }

  auto this_op = cuda::kDataCopy;
  auto q = DistEngine::Get()->GetTaskQueue(this_op);
  // receive the task data from sample process
  auto task = q->Recv();

  if (task) {
    Timer t0;
    DoGraphCopy(task);
    double graph_copy_time = t0.Passed();

    Timer t1;
    DoCacheIdCopy(task);
    double id_copy_time = t1.Passed();

    Timer t2;
    DoCacheFeatureCopy(task);
    DoGPULabelExtract(task);
    double cache_feat_copy_time = t2.Passed();

    LOG(DEBUG) << "Submit with cache: process task with key " << task->key;
    graph_pool->Submit(task->key, task);

    Profiler::Get().LogStep(
        task->key, kLogL1CopyTime,
        graph_copy_time + id_copy_time + cache_feat_copy_time);
    Profiler::Get().LogStep(task->key, kLogL2GraphCopyTime, graph_copy_time);
    Profiler::Get().LogStep(task->key, kLogL2IdCopyTime, id_copy_time);
    Profiler::Get().LogStep(task->key, kLogL2CacheCopyTime,
                            cache_feat_copy_time);
    Profiler::Get().LogEpochAdd(
        task->key, kLogEpochCopyTime,
        graph_copy_time + id_copy_time + cache_feat_copy_time);
  } else {
    std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
  }
  return true;
}

void DataCopySubLoop(int count) {
  LoopOnceFunction func;
  if (!RunConfig::UseGPUCache()) {
    func = RunDataCopySubLoopOnce;
  } else {
    func = RunCacheDataCopySubLoopOnce;
  }

  while ((count--) && !DistEngine::Get()->ShouldShutdown() && func());

  DistEngine::Get()->ReportThreadFinish();
}

} // namespace

void RunArch5LoopsOnce(DistType dist_type) {
  if (dist_type == DistType::Sample) {
    LOG(INFO) << "RunArch5LoopsOnce with Sample!";
    RunSampleSubLoopOnce();
  }
  else if (dist_type == DistType::Extract) {
    if (!RunConfig::UseGPUCache()) {
      LOG(INFO) << "RunArch5LoopsOnce with Extract no Cache!";
      RunDataCopySubLoopOnce();
    } else {
      LOG(INFO) << "RunArch5LoopsOnce with Extract Cache!";
      RunCacheDataCopySubLoopOnce();
    }
  } else {
    LOG(FATAL) << "dist type is illegal!";
  }
}

ExtractFunction GetArch5Loops() {
  return DataCopySubLoop;
}

}  // namespace dist
}  // namespace common
}  // namespace samgraph
