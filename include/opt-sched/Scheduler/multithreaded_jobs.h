/*******************************************************************************
Description:  Provides infrastructure used by multi-threaded schedulers
Author:       Paul McHugh
Created:      Jun. 2020
*******************************************************************************/

#ifndef OPTSCHED_MULTITHREADED_JOBS_H
#define OPTSCHED_MULTITHREADED_JOBS_H

#include "llvm/ADT/SmallVector.h"
#include "opt-sched/Scheduler/defines.h"
#include <cstdint>
#include <mutex>
#include <thread>
#include <condition_variable>

namespace llvm {
namespace opt_sched {

class Semaphore
{
public:

  Semaphore(int Count_ = 0) : Count{Count_}
  {}

  void post()
  {
    std::unique_lock<std::mutex> Lock(Mtx);
    ++Count;
    CV.notify_one();
  }

  void wait()
  {
    std::unique_lock<std::mutex> Lock(Mtx);
    while(Count == 0)
    {
      CV.wait(Lock);
    }

    --Count;
  }

private:

  std::mutex Mtx;
  std::condition_variable CV;
  int Count;
};

class WorkQueue;
void workerThreadSt(WorkQueue* parent);

class GenericJob {
private:
    WorkQueue* AssociatedQueue = nullptr;
public:
    virtual void perform() = 0;
    friend class WorkQueue;
};

class WorkQueue {
protected:
    SmallVector<std::thread, 32> Workers;
    SmallVector<GenericJob*, 32> Jobs; // protected by JobLock
    std::uint64_t JobsQueued = 0;  // protected by JobLock
    std::uint64_t JobsCompleted = 0;  // protected by JobLock
    bool BlockedForParent = false;  // protected by JobLock
    std::mutex JobLock;
    bool ShouldTerminate; // protected by TerminateMtx
    std::mutex TerminateMtx;
    Semaphore JobsAvailable; // initially there are no jobs
    Semaphore CompletedAllJobs; // this is a waiting semaphore
    friend void workerThreadSt(WorkQueue* parent);
public:
    explicit WorkQueue(unsigned threadCnt=0);
    WorkQueue(const WorkQueue&) = delete;
    WorkQueue operator=(const WorkQueue&) = delete;
    ~WorkQueue();
    void addJob(GenericJob *newJob);
    void blockUntilAllComplete();
};

} // namespace opt_sched
} // namespace llvm

#endif
