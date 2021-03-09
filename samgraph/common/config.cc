#include "config.h"

namespace samgraph {
namespace common {

const std::string Config::kMetaFile     = "meta.txt";
const std::string Config::kFeatFile     = "feat.bin";
const std::string Config::kLabelFile    = "label.bin";
const std::string Config::kInptrFile    = "indptr.bin";
const std::string Config::kIndicesFile  = "indices.bin";
const std::string Config::kTrainSetFile = "train_set.bin";
const std::string Config::kTestSetFile  = "test_set.bin";
const std::string Config::kValidSetFile = "valid_set.bin";

const std::string Config::kMetaNumNode     = "NUM_NODE";
const std::string Config::kMetaNumEdge     = "NUM_EDGE";
const std::string Config::kMetaFeatDim     = "NUM_CLASS";
const std::string Config::kMetaNumClass    = "NUM_CLASS";
const std::string Config::kMetaNumTrainSet = "NUM_TRAIN_SET";
const std::string Config::kMetaNumTestSet  = "NUM_TEST_SET"; 
const std::string Config::kMetaNumValidSet = "NUM_VALID_SET";

const std::unordered_map<int, size_t> Config::kQueueThreshold = {
    { ID_COPYH2D,    5 },
    { DEV_SAMPLE,    5 },
    { GRAPH_COPYD2D, 5 },
    { ID_COPYD2H,    5 },
    { FEAT_EXTRACT,  5 },
    { FEAT_COPYH2D,  5 },
    { SUBMIT,        5 },
};

} // namespace common
} // namespace samgraph