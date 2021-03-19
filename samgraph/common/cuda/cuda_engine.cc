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

#include "../types.h"
#include "../config.h"
#include "../logging.h"
#include "cuda_engine.h"

namespace samgraph{
namespace common {
namespace cuda {

bool SamGraphCudaEngine::_initialize = false;
bool SamGraphCudaEngine::_should_shutdown = false;

int SamGraphCudaEngine::_sample_device = 0;
int SamGraphCudaEngine::_train_device = 0;
std::string SamGraphCudaEngine::_dataset_path = "";
SamGraphDataset* SamGraphCudaEngine::_dataset = nullptr;
size_t SamGraphCudaEngine::_batch_size = 0;
std::vector<int> SamGraphCudaEngine::_fanout;
int SamGraphCudaEngine::_num_epoch = 0;

SamGraphTaskQueue* SamGraphCudaEngine::_queues[QueueNum] = {nullptr};
std::vector<std::thread*> SamGraphCudaEngine::_threads;

cudaStream_t* SamGraphCudaEngine::_sample_stream = nullptr;
cudaStream_t* SamGraphCudaEngine::_id_copy_host2device_stream = nullptr;
cudaStream_t* SamGraphCudaEngine::_graph_copy_device2device_stream = nullptr;
cudaStream_t* SamGraphCudaEngine::_id_copy_device2host_stream = nullptr;
cudaStream_t* SamGraphCudaEngine::_feat_copy_host2device_stream = nullptr;

ReadyTable* SamGraphCudaEngine::_submit_table = nullptr;
CpuExtractor* SamGraphCudaEngine::_cpu_extractor = nullptr;
RandomPermutation* SamGraphCudaEngine::_permutation = nullptr;
GraphPool* SamGraphCudaEngine::_graph_pool = nullptr;
std::shared_ptr<GraphBatch> SamGraphCudaEngine::_cur_graph_batch = nullptr;

std::atomic_int SamGraphCudaEngine::joined_thread_cnt;

void SamGraphCudaEngine::Init(std::string dataset_path, int sample_device, int train_device,
                          size_t batch_size, std::vector<int> fanout, int num_epoch) {
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

    CUDA_CALL(cudaSetDevice(_sample_device));
    CUDA_CALL(cudaStreamCreateWithFlags(_sample_stream, cudaStreamNonBlocking));
    CUDA_CALL(cudaStreamCreateWithFlags(_id_copy_host2device_stream, cudaStreamNonBlocking));
    CUDA_CALL(cudaStreamCreateWithFlags(_graph_copy_device2device_stream, cudaStreamNonBlocking));
    CUDA_CALL(cudaStreamCreateWithFlags(_id_copy_device2host_stream, cudaStreamNonBlocking));

    CUDA_CALL(cudaSetDevice(_train_device));
    CUDA_CALL(cudaStreamCreateWithFlags(_feat_copy_host2device_stream, cudaStreamNonBlocking));

    CUDA_CALL(cudaStreamSynchronize(*_sample_stream));
    CUDA_CALL(cudaStreamSynchronize(*_id_copy_host2device_stream));
    CUDA_CALL(cudaStreamSynchronize(*_graph_copy_device2device_stream));
    CUDA_CALL(cudaStreamSynchronize(*_id_copy_device2host_stream));

    CUDA_CALL(cudaStreamSynchronize(*_feat_copy_host2device_stream));

    _submit_table = new ReadyTable(2, "SUBMIT");

    _cpu_extractor = new CpuExtractor();

    // Create queues
    for (int i = 0; i < QueueNum; i++) {
        SAM_LOG(DEBUG) << "Create task queue" << i;
        auto type = static_cast<QueueType>(i);
        SamGraphCudaEngine::CreateTaskQueue(type);
    }

    _permutation = new RandomPermutation(_dataset->train_set, _num_epoch, _batch_size, false);
    _graph_pool = new GraphPool(Config::kGraphPoolThreshold);

    joined_thread_cnt = 0;

    _initialize = true;
}

void SamGraphCudaEngine::Start(const std::vector<LoopFunction> &func) {
    // Start background threads
    for (size_t i = 0; i < func.size(); i++) {
        _threads.push_back(new std::thread(func[i]));
    }
    SAM_LOG(DEBUG) << "Started " << func.size() << " background threads.";
}

void SamGraphCudaEngine::Shutdown() {
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

    if (_submit_table) {
        delete _submit_table;
        _submit_table = nullptr;
    }

    if (_cpu_extractor) {
        delete _cpu_extractor;
        _cpu_extractor = nullptr;
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
        CUDA_CALL(cudaStreamSynchronize(*_sample_stream));
        CUDA_CALL(cudaStreamDestroy(*_sample_stream));
        free(_sample_stream);
        _sample_stream = nullptr;
    }

    if (_id_copy_host2device_stream) {
        CUDA_CALL(cudaStreamSynchronize(*_id_copy_host2device_stream));
        CUDA_CALL(cudaStreamDestroy(*_id_copy_host2device_stream));
        free(_id_copy_host2device_stream);
        _id_copy_host2device_stream = nullptr;
    }

    if (_graph_copy_device2device_stream) {
        CUDA_CALL(cudaStreamSynchronize(*_graph_copy_device2device_stream));
        CUDA_CALL(cudaStreamDestroy(*_graph_copy_device2device_stream));
        free(_graph_copy_device2device_stream);
        _graph_copy_device2device_stream = nullptr;
    }

    if (_id_copy_device2host_stream) {
        CUDA_CALL(cudaStreamSynchronize(*_id_copy_device2host_stream));
        CUDA_CALL(cudaStreamDestroy(*_id_copy_device2host_stream));
        free(_id_copy_device2host_stream);
        _id_copy_device2host_stream = nullptr;
    }

    if (_feat_copy_host2device_stream) {
        CUDA_CALL(cudaStreamSynchronize(*_feat_copy_host2device_stream));
        CUDA_CALL(cudaStreamDestroy(*_feat_copy_host2device_stream));
        free(_feat_copy_host2device_stream);
        _feat_copy_host2device_stream = nullptr;
    }

    if (_permutation) {
        delete _permutation;
        _permutation = nullptr;
    }

    if (_graph_pool) {
        delete _graph_pool;
        _graph_pool = nullptr;
    }

    _threads.clear();
    joined_thread_cnt = 0;
    _initialize = false;
    _should_shutdown = false;
}

void SamGraphCudaEngine::CreateTaskQueue(QueueType queueType) {
    if (!_queues[queueType]) {
        _queues[queueType] = new SamGraphTaskQueue(queueType, Config::kQueueThreshold.at(queueType));
    }
}

void SamGraphCudaEngine::LoadGraphDataset() {
    // Load graph dataset from disk by mmap and copy the graph
    // topology data into the target CUDA device. 
    _dataset = new SamGraphDataset();
    std::unordered_map<std::string, size_t> meta;

    if (_dataset_path.back() != '/') {
        _dataset_path.push_back('/');
    }

    // Parse the meta data
    std::ifstream meta_file(_dataset_path + Config::kMetaFile);
    std::string line;
    while(std::getline(meta_file, line)) {
        std::istringstream iss(line);
        std::vector<std::string> kv {std::istream_iterator<std::string>{iss}, std::istream_iterator<std::string>{}};

        if (kv.size() < 2) {
            break;
        }

        meta[kv[0]] = std::stoull(kv[1]);
    }

    SAM_CHECK(meta.count(Config::kMetaNumNode) > 0);
    SAM_CHECK(meta.count(Config::kMetaNumEdge) > 0);
    SAM_CHECK(meta.count(Config::kMetaFeatDim) > 0);
    SAM_CHECK(meta.count(Config::kMetaNumClass) > 0);
    SAM_CHECK(meta.count(Config::kMetaNumTrainSet) > 0);
    SAM_CHECK(meta.count(Config::kMetaNumTestSet) > 0);
    SAM_CHECK(meta.count(Config::kMetaNumValidSet) > 0);

    _dataset->num_node  = meta[Config::kMetaNumNode];
    _dataset->num_edge  = meta[Config::kMetaNumEdge];
    _dataset->num_class = meta[Config::kMetaNumClass];

    _dataset->indptr    = Tensor::FromMmap(_dataset_path + Config::kInptrFile, DataType::kSamI32,
                                           {meta[Config::kMetaNumNode] + 1}, _sample_device, "dataset.indptr");
    _dataset->indices   = Tensor::FromMmap(_dataset_path + Config::kIndicesFile, DataType::kSamI32,
                                          {meta[Config::kMetaNumEdge]}, _sample_device, "dataset.indices");
    _dataset->feat      = Tensor::FromMmap(_dataset_path + Config::kFeatFile, DataType::kSamF32,
                                          {meta[Config::kMetaNumNode], meta[Config::kMetaFeatDim]}, CPU_DEVICE_MMAP_ID, "dataset.feat");
    _dataset->label     = Tensor::FromMmap(_dataset_path + Config::kLabelFile, DataType::kSamI64,
                                          {meta[Config::kMetaNumNode]}, CPU_DEVICE_MMAP_ID, "dataset.label");
    _dataset->train_set = Tensor::FromMmap(_dataset_path + Config::kTrainSetFile, DataType::kSamI32,
                                          {meta[Config::kMetaNumTrainSet]}, CPU_DEVICE_ID, "dataset.train_set");
    _dataset->test_set  = Tensor::FromMmap(_dataset_path + Config::kTestSetFile, DataType::kSamI32,
                                          {meta[Config::kMetaNumTestSet]}, CPU_DEVICE_ID, "dataset.test_set");
    _dataset->valid_set = Tensor::FromMmap(_dataset_path + Config::kValidSetFile, DataType::kSamI32,
                                          {meta[Config::kMetaNumValidSet]}, CPU_DEVICE_ID, "dataset.valid_set");

    SAM_LOG(INFO) << "SamGraph loaded dataset(" << _dataset_path <<  ") successfully";
}

bool SamGraphCudaEngine::IsAllThreadFinish(int total_thread_num) {
  int k = joined_thread_cnt.fetch_add(0);
  return (k == total_thread_num);
};

} // namespace cuda
} // namespace common
} // namespace samgraph