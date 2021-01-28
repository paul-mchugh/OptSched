#include "opt-sched/Scheduler/multithreaded_jobs.h"

using namespace llvm::opt_sched;

thread_local WorkQueue* ParentWQ = nullptr;

//GenericJob subclass DummyJob used for shutting down the WorkQueue
class DummyJob : public GenericJob {
  void perform() {}
};

void llvm::opt_sched::workerThreadSt(WorkQueue* parent)
{
  // Save the WorkQueue this thread is associated with
  ParentWQ = parent;

  // Here is the work loop where we handle jobs
  while(true) {

    //wait for jobs to be available on the semaphore
    ParentWQ->JobsAvailable.wait();

    //check if we should terminate
    bool LocalShouldTerminate = false;
    ParentWQ->TerminateMtx.lock(); // start should terminate CS
    LocalShouldTerminate = ParentWQ->ShouldTerminate;
    ParentWQ->TerminateMtx.unlock(); // close should terminate CS
    if (LocalShouldTerminate)
      return;

    //since we know that there are jobs available we can grab one
    GenericJob *Job = nullptr;
    ParentWQ->JobLock.lock(); // start manipulate queue CS
    Job = ParentWQ->Jobs.pop_back_val();
    ParentWQ->JobLock.unlock(); // close manipulate queue CS

    //run the job we got from the queue
    Job->perform();

    // now that the job is done we increment the number of jobs completed
    // if all jobs queued are completed then we are done and can unblock
    // the parent thread
    ParentWQ->JobLock.lock(); // start manipulate queue CS
    ++ParentWQ->JobsCompleted;
    // check if we are done
    if (ParentWQ->JobsCompleted==ParentWQ->JobsQueued) {
      ParentWQ->BlockedForParent=false;
      ParentWQ->CompletedAllJobs.post();
    }
    ParentWQ->JobLock.unlock(); // close manipulate queue CS
  }
}

WorkQueue::WorkQueue(unsigned ThreadCnt)
{
  //if threadCnt is set to 0 then set it to the number of physical cores
  if(!ThreadCnt)
    ThreadCnt = std::thread::hardware_concurrency();

  //launch the threads
  for(unsigned i=0;i<ThreadCnt;++i)
    Workers.push_back(std::move(std::thread(workerThreadSt, this)));
}

void WorkQueue::addJob(GenericJob *NewJob)
{
  // newJob should not be part of anyworkqueue yet
  assert(!NewJob->AssociatedQueue);
  //set the newJob to be affiliated with this work queue
  NewJob->AssociatedQueue = this;

  JobLock.lock(); // start manipulate queue CS
  //You can not add jobs while blocked for the parent
  assert(!BlockedForParent);
  Jobs.push_back(NewJob);
  ++JobsQueued;
  JobsAvailable.post();
  JobLock.unlock(); // close manipulate queue CS
}

void WorkQueue::blockUntilAllComplete() {
  bool LocalBlocking = false;
  JobLock.lock(); // start manipulate queue CS

  // first assert that no other thread is blocked. Only one thread can block
  assert(!BlockedForParent);

  // first we check if there is any work in the queue to block for
  // if there arent then we return immediately
  if (JobsCompleted!=JobsQueued) {
    // if there are jobs remaining...
    // we set the queue as blocking the parent
    LocalBlocking = BlockedForParent = true;
  }
  JobLock.unlock(); // close manipulate queue CS

  // now we wait on the CompletedAllJobs semaphore if LocalBlocking is set
  // we need to check LocalBlocking since after JobLock unlocked
  // the last job could have been completed and BlockedForParent
  // would have been reset to false.  That would result in the completed
  // semaphore getting an extra count.
  if (LocalBlocking)
    CompletedAllJobs.wait();
}

WorkQueue::~WorkQueue() {
  //we need to shut down all the worker threads

  //first set the terminate variable
  TerminateMtx.lock(); // start should terminate CS
  ShouldTerminate = true;
  TerminateMtx.unlock(); // close should terminate CS

  // now we create dummy jobs for all the threads since they likely are
  // waiting on a semaphore for work.
  // executing the addition of a job will wake them up.
  // They will then terminate once they see that terminate is set.
  unsigned int workerCnt = Workers.size();
  SmallVector<DummyJob, 32> dummyJobs(workerCnt);
  for(unsigned int i = 0; i < workerCnt; ++i)
    addJob(&dummyJobs[i]);

  //join all the workers
  for(std::thread &t : Workers)
    t.join();

}
