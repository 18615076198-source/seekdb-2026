/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#ifndef OCEANBASE_LIB_VECTOR_CUVS_SEARCH_WORKER_H_
#define OCEANBASE_LIB_VECTOR_CUVS_SEARCH_WORKER_H_

#include <pthread.h>
#include <mutex>
#include <condition_variable>

namespace oceanbase {
namespace common {
namespace obvsag {

// CAGRA needs a larger stack than an OB worker provides. Keep the same OS
// thread across synchronous searches to reuse HIP's thread-local resources.
class CuvsSearchWorker {
public:
  CuvsSearchWorker() : started_(false), stop_(false), ready_(false),
      done_(false), job_(nullptr), arg_(nullptr)
  {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) == 0) {
      if (pthread_attr_setstacksize(&attr, 32UL * 1024 * 1024) == 0) {
        started_ = pthread_create(&thread_, &attr, &CuvsSearchWorker::entry, this) == 0;
      }
      pthread_attr_destroy(&attr);
    }
  }

  ~CuvsSearchWorker()
  {
    // No new submissions are allowed once destruction starts.
    std::lock_guard<std::mutex> submit_guard(submit_mu_);
    if (started_) {
      {
        std::lock_guard<std::mutex> guard(mu_);
        stop_ = true;
      }
      work_cv_.notify_one();
      pthread_join(thread_, nullptr);
    }
  }

  bool run(void *(*job)(void *), void *arg)
  {
    // Keep the caller's stack-owned job alive until THIS job has completed,
    // even if callers later stop holding the adaptor's registry mutex.
    std::lock_guard<std::mutex> submit_guard(submit_mu_);
    if (!started_ || job == nullptr) { return false; }
    std::unique_lock<std::mutex> lock(mu_);
    job_ = job;
    arg_ = arg;
    ready_ = true;
    done_ = false;
    work_cv_.notify_one();
    done_cv_.wait(lock, [this] { return done_; });
    return true;
  }

  CuvsSearchWorker(const CuvsSearchWorker &) = delete;
  CuvsSearchWorker &operator=(const CuvsSearchWorker &) = delete;

private:
  static void *entry(void *arg)
  {
    static_cast<CuvsSearchWorker *>(arg)->loop();
    return nullptr;
  }

  void loop()
  {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
      work_cv_.wait(lock, [this] { return ready_ || stop_; });
      if (stop_) { return; }
      auto job = job_;
      void *arg = arg_;
      ready_ = false;
      lock.unlock();
      job(arg);
      lock.lock();
      job_ = nullptr;
      arg_ = nullptr;
      done_ = true;
      done_cv_.notify_one();
    }
  }

  pthread_t thread_;
  bool started_;
  bool stop_;
  bool ready_;
  bool done_;
  void *(*job_)(void *);
  void *arg_;
  std::mutex submit_mu_;
  std::mutex mu_;
  std::condition_variable work_cv_;
  std::condition_variable done_cv_;
};

} // namespace obvsag
} // namespace common
} // namespace oceanbase
#endif
