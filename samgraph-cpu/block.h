#pragma once

#include <cstdlib>
#include <cstdint>
#include <vector>

#include "graph_storage.h"

struct Block {
    int num_picks = 0;
    int feat_dim = 0;

    size_t num_src_nodes = 0;
    size_t num_dst_nodes = 0;

    std::shared_ptr<COO> raw_block;

    std::vector<uint32_t> seed_index;
    std::vector<uint32_t> node_index;

    float *block_features = nullptr;
    uint32_t *block_label = nullptr;

    std::shared_ptr<COO> coo_ptr;
    std::shared_ptr<CSR> csr_ptr, csc_ptr;

    ~Block() {
        if (block_features) {
            free(block_features);
        }

        if (block_label) {
            free(block_label);
        }
    }
};
