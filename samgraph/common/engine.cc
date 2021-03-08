#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <iterator>
#include <algorithm>
#include <tuple>
#include <numeric>
#include <functional>

#include "types.h"
#include "config.h"
#include "logging.h"
#include "engine.h"

namespace samgraph{
namespace common {

bool SamGraphEngine::_initialize = false;
bool SamGraphEngine::_should_shutdown = false;

int SamGraphEngine::_sample_device = 0;
int SamGraphEngine::_train_device = 0;
std::string SamGraphEngine::_dataset_path = "";
SamGraphDataset* SamGraphEngine::_dataset = nullptr;
int SamGraphEngine::_batch_size = 0;
std::vector<int> SamGraphEngine::_fanout;
int SamGraphEngine::_num_epoch = 0;

volatile SamGraphTaskQueue* SamGraphEngine::_queues[QueueNum] = {nullptr};
static std::vector<std::thread*> SamGraphEngine_threads;

cudaStream_t* SamGraphEngine::_sample_stream = nullptr;
cudaStream_t* SamGraphEngine::_id_copy_host2device_stream = nullptr;
cudaStream_t* SamGraphEngine::_graph_copy_device2device_stream = nullptr;
cudaStream_t* SamGraphEngine::_id_copy_device2host_stream = nullptr;
cudaStream_t* SamGraphEngine::_feat_copy_host2device_stream = nullptr;

RandomPermutation* SamGraphEngine::_permutation = nullptr;
GraphPool* SamGraphEngine::_graph_pool = nullptr;

std::atomic_int SamGraphEngine::joined_thread_cnt;

void SamGraphEngine::Init(std::string dataset_path, int sample_device, int train_device,
                          int batch_size, std::vector<int> fanout, int num_epoch) {
    if (_initialize) {
        return;
    }

    _sample_device = sample_device;
    _train_device = train_device;
    _dataset_path = dataset_path;
    _batch_size = batch_size;
    _fanout = fanout;
    _num_epoch = num_epoch;

    // Load the target graph data
    LoadGraphDataset();

    // Create CUDA streams
    _sample_stream = (cudaStream_t*) malloc(sizeof(cudaStream_t));
    _id_copy_host2device_stream = (cudaStream_t*) malloc(sizeof(cudaStream_t));
    _graph_copy_device2device_stream = (cudaStream_t*) malloc(sizeof(cudaStream_t));
    _id_copy_device2host_stream = (cudaStream_t*) malloc(sizeof(cudaStream_t));
    _feat_copy_host2device_stream = (cudaStream_t*) malloc(sizeof(cudaStream_t));

    CUDA_CALL(cudaStreamCreateWithFlags(_sample_stream, cudaStreamNonBlocking));
    CUDA_CALL(cudaStreamCreateWithFlags(_id_copy_host2device_stream, cudaStreamNonBlocking));
    CUDA_CALL(cudaStreamCreateWithFlags(_graph_copy_device2device_stream, cudaStreamNonBlocking));
    CUDA_CALL(cudaStreamCreateWithFlags(_id_copy_device2host_stream, cudaStreamNonBlocking));
    CUDA_CALL(cudaStreamCreateWithFlags(_id_copy_device2host_stream, cudaStreamNonBlocking));
    CUDA_CALL(cudaStreamCreateWithFlags(_feat_copy_host2device_stream, cudaStreamNonBlocking));

    CUDA_CALL(cudaStreamSynchronize(*_sample_stream));
    CUDA_CALL(cudaStreamSynchronize(*_id_copy_host2device_stream));
    CUDA_CALL(cudaStreamSynchronize(*_graph_copy_device2device_stream));
    CUDA_CALL(cudaStreamSynchronize(*_id_copy_device2host_stream));
    CUDA_CALL(cudaStreamSynchronize(*_feat_copy_host2device_stream));

    // Create queues
    for (int i = 0; i < QueueNum; i++) {
        SAM_LOG(DEBUG) << "Create task queue" << i;
        auto type = static_cast<QueueType>(i);
        SamGraphEngine::CreateTaskQueue(type);
    }

    _permutation = new RandomPermutation(_dataset->train_set, _batch_size, false);
    _graph_pool = new GraphPool();

    joined_thread_cnt = 0;

    _initialize = true;
}

void SamGraphEngine::Start(const std::vector<LoopFunction> &func) {
    // Start background threads
    for (size_t i = 0; i < func.size(); i++) {
        _threads.push_back(new std::thread(func[i]));
    }
    SAM_LOG(DEBUG) << "Started" << func.size() << " background threads.";
}

void SamGraphEngine::Shutdown() {
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
    for (size_t i = 0; i < QueueNum; i++) {
        if (_queues[i]) {
            delete _queues[i];
            _queues[i] = nullptr;
        }
    }

    delete _dataset;

    if (_sample_stream) {
        CUDA_CALL(cudaStreamDestroy(*_sample_stream));
        free(_sample_stream);
        _sample_stream = nullptr;
    }

    if (_id_copy_host2device_stream) {
        CUDA_CALL(cudaStreamDestroy(*_id_copy_host2device_stream));
        free(_id_copy_host2device_stream);
        _id_copy_host2device_stream = nullptr;
    }

    if (_graph_copy_device2device_stream) {
        CUDA_CALL(cudaStreamDestroy(*_graph_copy_device2device_stream));
        free(_graph_copy_device2device_stream);
        _graph_copy_device2device_stream = nullptr;
    }

    if (_id_copy_device2host_stream) {
        CUDA_CALL(cudaStreamDestroy(*_id_copy_device2host_stream));
        free(_id_copy_device2host_stream);
        _id_copy_device2host_stream = nullptr;
    }

    if (_feat_copy_host2device_stream) {
        CUDA_CALL(cudaStreamDestroy(*_feat_copy_host2device_stream));
        free(_feat_copy_host2device_stream);
        _feat_copy_host2device_stream = nullptr;
    }

    delete _permutation;
    _permutation = nullptr;
    delete _graph_pool;
    _graph_pool = nullptr;

    _threads.clear();
    joined_thread_cnt = 0;
    _initialize = false;
    _should_shutdown = false;
}

void SamGraphEngine::CreateTaskQueue(QueueType queueType) {
    if (!_queues[queueType]) {
        _queues[queueType] = new SamGraphTaskQueue(queueType, kQueueThreshold[queueType]);
    }
}

void SamGraphEngine::LoadGraphDataset() {
    // Load graph dataset from disk by mmap and copy the graph
    // topology data into the target CUDA device. 
    _dataset = new SamGraphDataset();
    std::unordered_map<std::string, size_t> meta;

    if (_dataset_path.back() != '/') {
        _dataset_path.push_back('/');
    }

    // Parse the meta data
    std::ifstream meta_file(_dataset_path + kMetaFile);
    std::string line;
    while(std::getline(meta_file, line)) {
        std::istringstream iss(line);
        std::vector<std::string> kv {std::istream_iterator<std::string>{iss}, std::istream_iterator<std::string>{}};

        if (kv.size() < 2) {
            break;
        }

        meta[kv[0]] = std::stoull(kv[1]);
    }

    SAM_CHECK(meta.count(kMetaNumNode) > 0);
    SAM_CHECK(meta.count(kMetaNumEdge) > 0);
    SAM_CHECK(meta.count(kMetaFeatDim) > 0);
    SAM_CHECK(meta.count(kMetaNumClass) > 0);
    SAM_CHECK(meta.count(KMetaNumTrainSet) > 0);
    SAM_CHECK(meta.count(kMetaNumTestSet) > 0);
    SAM_CHECK(meta.count(kMetaNumValidSet) > 0);

    _dataset->num_node  = meta[kMetaNumNode];
    _dataset->num_edge  = meta[kMetaNumEdge];
    _dataset->num_class = meta[kMetaNumClass];

    _dataset->indptr    = Tensor::FromMmap(_dataset_path + kInptrFile, DataType::kSamI32,
                                           {meta[kMetaNumNode] + 1}, _sample_device);
    _dataset->indices   = Tensor::FromMmap(_dataset_path + kIndicesFile, DataType::kSamI32,
                                          {meta[kMetaNumEdge]}, _sample_device);
    _dataset->feat      = Tensor::FromMmap(_dataset_path + kFeatFile, DataType::kSamF32,
                                          {meta[kMetaNumNode], meta[kMetaFeatDim]}, CPU_DEVICE_ID);
    _dataset->label     = Tensor::FromMmap(_dataset_path + kLabelFile, DataType::kSamI32,
                                          {meta[kMetaNumNode]}, CPU_DEVICE_ID);
    _dataset->train_set = Tensor::FromMmap(_dataset_path + kTrainSetFile, DataType::kSamI32,
                                          {meta[KMetaNumTrainSet]}, CPU_DEVICE_ID);
    _dataset->test_set  = Tensor::FromMmap(_dataset_path + kTestSetFile, DataType::kSamI32,
                                          {meta[kMetaNumTestSet]}, CPU_DEVICE_ID);
    _dataset->valid_set = Tensor::FromMmap(_dataset_path + kValidSetFile, DataType::kSamI32,
                                          {meta[kMetaNumValidSet]}, CPU_DEVICE_ID);
}

bool SamGraphEngine::IsAllThreadFinish(int total_thread_num) {
  int k = joined_thread_cnt.fetch_add(0);
  return (k == total_thread_num);
};

} // namespace common
} // namespace samgraph