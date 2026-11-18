// Standalone CPU regression test for the large-stack GPU dispatch worker.
// g++ -std=c++11 -O2 -pthread -Isrc/oblib tools/hipvs_bridge/test_search_worker.cpp -o test_search_worker
#include "lib/vector/cuvs_search_worker.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

using oceanbase::common::obvsag::CuvsSearchWorker;

struct Job {
  int input;
  int output;
  pthread_t thread;
  std::atomic<int> *active;
};

static void *execute(void *arg)
{
  Job &job = *static_cast<Job *>(arg);
  assert(job.active->fetch_add(1) == 0);
  // A real CAGRA callback needs more stack than the OB caller provides.
  volatile unsigned char scratch[2 * 1024 * 1024];
  for (size_t i = 0; i < sizeof(scratch); i += 4096) { scratch[i] = 7; }
  job.thread = pthread_self();
  job.output = job.input * 3 + scratch[0];
  assert(job.active->fetch_sub(1) == 1);
  return nullptr;
}

int main()
{
  std::atomic<int> active(0);
  for (int lifetime = 0; lifetime < 3; ++lifetime) {
    CuvsSearchWorker worker;
    assert(!worker.run(nullptr, nullptr));
    Job first{11, -1, pthread_t(), &active};
    assert(worker.run(execute, &first));
    assert(first.output == 40);
    assert(!pthread_equal(first.thread, pthread_self()));
    std::vector<std::thread> callers;
    for (int caller = 0; caller < 8; ++caller) {
      callers.emplace_back([&worker, &active, &first, caller] {
        for (int i = 0; i < 250; ++i) {
          Job job{caller * 250 + i, -1, pthread_t(), &active};
          assert(worker.run(execute, &job));
          assert(job.output == job.input * 3 + 7);
          assert(pthread_equal(first.thread, job.thread));
        }
      });
    }
    for (auto &caller : callers) { caller.join(); }
    assert(active.load() == 0);
    // Destruction must wake and join an idle worker without hanging.
  }
  std::puts("PASS: 6003 jobs, 8 concurrent callers, stable worker, 2 MiB stack, clean shutdown");
}
