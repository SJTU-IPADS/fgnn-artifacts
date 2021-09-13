#pragma once

#ifndef SAMGRAPH_DIST_LOOPS_H
#define SAMGRAPH_DIST_LOOPS_H

#include "../common.h"
#include "dist_engine.h"

namespace samgraph {
namespace common {
namespace dist {

void RunArch5LoopsOnce(DistType dist_type);

// common steps
TaskPtr DoShuffle();
void DoGPUSample(TaskPtr task);
void DoGraphCopy(TaskPtr task);
void DoIdCopy(TaskPtr task);
void DoCPUFeatureExtract(TaskPtr task);
void DoFeatureCopy(TaskPtr task);

void DoCacheIdCopy(TaskPtr task);
void DoCacheFeatureCopy(TaskPtr task);
void DoGPULabelExtract(TaskPtr task);

typedef void (*ExtractFunction)(int);
ExtractFunction GetArch5Loops();

}  // namespace dist
}  // namespace common
}  // namespace samgraph

#endif // SAMGRAPH_DIST_LOOPS_H
