//===- GCNOptSched.cpp - AMDGCN Combinatorial scheudler -------------------===//
//
// Implements a combinatorial scheduling strategy for AMDGCN.
//
//===----------------------------------------------------------------------===//

#include "GCNOptSched.h"
#include "AMDGPUMacroFusion.h"
#include "GCNSchedStrategy.h"
#include "SIMachineFunctionInfo.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "optsched"

using namespace llvm::opt_sched;

// FIXME: Temporary, eliminate
static cl::opt<bool>
    GCNLimitOccWithHints("gcn-limit-occ-with-hints",
                         cl::desc("Limit occpancy target using perf hints."),
                         cl::init(false), cl::Hidden);

static ScheduleDAGInstrs *createOptSchedGCN(MachineSchedContext *C) {
  return new ScheduleDAGOptSchedGCN(
      C, llvm::make_unique<GCNMaxOccupancySchedStrategy>(C));
}

// Register the machine scheduler.
static MachineSchedRegistry
    OptSchedMIRegistry("gcn-optsched", "Use the GCN OptSched scheduler.",
                       createOptSchedGCN);

ScheduleDAGOptSchedGCN::ScheduleDAGOptSchedGCN(
    llvm::MachineSchedContext *C, std::unique_ptr<MachineSchedStrategy> S)
    : ScheduleDAGOptSched(C, std::move(S)) {}

void ScheduleDAGOptSchedGCN::initSchedulers() {
  // Add DAG mutations that apply to both GCN and OptSched DAG's

  addMutation(createLoadClusterDAGMutation(TII, TRI));
  addMutation(createStoreClusterDAGMutation(TII, TRI));
  addMutation(createAMDGPUMacroFusionDAGMutation());

  // Add passes

  // First
  // SchedPasses.push_back(GCNMaxOcc);
  // Second
  SchedPasses.push_back(OptSchedMaxOcc);
  // Third
  SchedPasses.push_back(OptSchedBalanced);
}

// Record scheduling regions.
void ScheduleDAGOptSchedGCN::schedule() {
  Regions.push_back(std::make_pair(RegionBegin, RegionEnd));
  return;
}

// Execute scheduling passes.
// Partially copied GCNScheduleDAGMILive::finalizeSchedule
void ScheduleDAGOptSchedGCN::finalizeSchedule() {
  initSchedulers();

  for (const SchedPassStrategy &S : SchedPasses) {
    MachineBasicBlock *MBB = nullptr;

    for (auto &Region : Regions) {
      RegionBegin = Region.first;
      RegionEnd = Region.second;

      if (RegionBegin->getParent() != MBB) {
        if (MBB)
          finishBlock();
        MBB = RegionBegin->getParent();
        startBlock(MBB);
      }

      unsigned NumRegionInstrs = std::distance(begin(), end());
      enterRegion(MBB, begin(), end(), NumRegionInstrs);

      // Skip empty scheduling regions (0 or 1 schedulable instructions).
      if (begin() == end() || begin() == std::prev(end())) {
        exitRegion();
        continue;
      }

      LLVM_DEBUG(dbgs() << "********** MI Scheduling **********\n");
      LLVM_DEBUG(dbgs() << MF.getName() << ":" << printMBBReference(*MBB) << " "
                        << MBB->getName() << "\n  From: " << *begin()
                        << "    To: ";
                 if (RegionEnd != MBB->end()) dbgs() << *RegionEnd;
                 else dbgs() << "End";
                 dbgs() << " RegionInstrs: " << NumRegionInstrs << '\n');

      runSchedPass(S);

      Region = std::make_pair(RegionBegin, RegionEnd);
      exitRegion();
    }
    finishBlock();
  }
}

void ScheduleDAGOptSchedGCN::runSchedPass(SchedPassStrategy S) {
  switch (S) {
  case GCNMaxOcc:
    scheduleGCNMaxOcc();
    break;
  case OptSchedMaxOcc:
    scheduleOptSchedMaxOcc();
    break;
  case OptSchedBalanced:
    scheduleOptSchedBalanced();
    break;
  }
}

void ScheduleDAGOptSchedGCN::scheduleGCNMaxOcc() {
  auto &S = (GCNMaxOccupancySchedStrategy &)*SchedImpl;
  if (GCNLimitOccWithHints) {
    const auto &MFI = *MF.getInfo<SIMachineFunctionInfo>();
    S.setTargetOccupancy(MFI.getMinAllowedOccupancy());
  }

  ScheduleDAGMILive::schedule();
}

void ScheduleDAGOptSchedGCN::scheduleOptSchedMaxOcc() {
  latencyPrecision = LTP_UNITY;
  heurSchedType = SCHED_LIST;
  spillCostFunction = SCF_TARGET;

  ScheduleDAGOptSched::schedule();
}

void ScheduleDAGOptSchedGCN::scheduleOptSchedBalanced() {
  latencyPrecision = LTP_ROUGH;
  // Force the input to the balanced scheduler to be the sequential order of the
  // (hopefully) good max occupancy schedule. We don’t want the list scheduler
  // to mangle the input because of latency or resource constraints.
  heurSchedType = SCHED_SEQ;
  spillCostFunction = SCF_TARGET;

  ScheduleDAGOptSched::schedule();
}
