#include "constant.h"

namespace samgraph {
namespace common {

const std::string Constant::kMetaFile = "meta.txt";
const std::string Constant::kFeatFile = "feat.bin";
const std::string Constant::kLabelFile = "label.bin";
const std::string Constant::kIndptrFile = "indptr.bin";
const std::string Constant::kIndicesFile = "indices.bin";
const std::string Constant::kTrainSetFile = "train_set.bin";
const std::string Constant::kTestSetFile = "test_set.bin";
const std::string Constant::kValidSetFile = "valid_set.bin";

const std::string Constant::kProbTableFile = "prob_table.bin";
const std::string Constant::kAliasTableFile = "alias_table.bin";

const std::string Constant::kInDegreeFile = "in_degrees.bin";
const std::string Constant::kOutDegreeFile = "out_degrees.bin";
const std::string Constant::kCacheByDegreeFile = "cache_by_degree.bin";
const std::string Constant::kCacheByHeuristicFile = "cache_by_heuristic.bin";
const std::string Constant::kCacheByDegreeHopFile = "cache_by_degree_hop.bin";
const std::string Constant::kCacheByFakeOptimalFile = "cache_by_fake_optimal.bin";

const std::string Constant::kMetaNumNode = "NUM_NODE";
const std::string Constant::kMetaNumEdge = "NUM_EDGE";
const std::string Constant::kMetaFeatDim = "FEAT_DIM";
const std::string Constant::kMetaNumClass = "NUM_CLASS";
const std::string Constant::kMetaNumTrainSet = "NUM_TRAIN_SET";
const std::string Constant::kMetaNumTestSet = "NUM_TEST_SET";
const std::string Constant::kMetaNumValidSet = "NUM_VALID_SET";

const std::string Constant::kEnvProfileLevel = "SAMGRAPH_PROFILE_LEVEL";
const std::string Constant::kEnvProfileCuda = "SAMGRAPH_PROFILE_CUDA";
const std::string Constant::kEnvLogNodeAccess = "SAMGRAPH_LOG_NODE_ACCESS";
const std::string Constant::kEnvLogNodeAccessSimple = "SAMGRAPH_LOG_NODE_ACCESS_SIMPLE";
const std::string Constant::kEnvSanityCheck = "SAMGRAPH_SANITY_CHECK";
const std::string Constant::kBarrierEpoch = "SAMGRAPH_BARRIER_EPOCH";
const std::string Constant::kEnvDumpTrace = "SAMGRAPH_DUMP_TRACE";

const std::string Constant::kNodeAccessLogFile = "node_access";
const std::string Constant::kNodeAccessFrequencyFile = "node_access_frequency";
const std::string Constant::kNodeAccessOptimalCacheHitFile = "node_access_optimal_cache_hit";
const std::string Constant::kNodeAccessOptimalCacheBinFile = "node_access_optimal_cache_bin";
const std::string Constant::kNodeAccessSimilarityFile =
    "node_access_similarity";
const std::string Constant::kNodeAccessPreSampleSimFile = "node_access_presample";
const std::string Constant::kNodeAccessFileSuffix = ".txt";

}  // namespace common
}  // namespace samgraph