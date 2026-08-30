#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "background_job_state.h"

// Generic single-slot background job: a mutex-guarded "start (no-op if
// already running) / finish / read status" wrapper around
// BackgroundJobState's pure transition rules (lib/background_job_state).
// Used by webserver_cameras.cpp for both the "Test all cameras" and
// "Search network for cameras" buttons, which used to each hand-write an
// identical bool inProgress/bool hasResult/std::vector<T> results/mutex
// block - one implementation now, instead of two independently-typed
// copies of the same locking logic.
//
// T is whatever the job produces - a std::vector<CameraTestResult>, a
// std::vector<DiscoveredCamera>. Must be default-constructible.
template <typename T>
class BackgroundJob {
 public:
  BackgroundJob() : mutex_(xSemaphoreCreateMutex()) {}

  // False (a no-op) if a run is already in progress - the caller must NOT
  // spawn a second overlapping task in that case. True means the caller
  // now owns starting a task that eventually calls finish().
  bool tryStart() {
    bool started;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    started = shouldStartBackgroundJob(state_);
    if (started) state_ = markBackgroundJobStarted(state_);
    xSemaphoreGive(mutex_);
    return started;
  }

  // Call this if tryStart() returned true but the caller then failed to
  // actually launch the task (e.g. xTaskCreate returned pdFAIL) - rolls
  // inProgress back to false via markBackgroundJobStartFailed
  // (background_job_state.h) so the NEXT tryStart() isn't permanently
  // wedged waiting for a finish() that will never come. Does not touch
  // hasResult/result - see that function's own comment.
  void cancelStart() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    state_ = markBackgroundJobStartFailed(state_);
    xSemaphoreGive(mutex_);
  }

  // Called once, by the background task itself, when the work is done.
  void finish(T result) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    result_ = result;
    state_ = markBackgroundJobFinished(state_);
    xSemaphoreGive(mutex_);
  }

  struct Status {
    bool inProgress;
    bool hasResult;
    T result; // only meaningful if hasResult
  };

  // Safe to call from any task. result is copied out under the lock, same
  // as both hand-written versions this replaced did - the caller renders
  // it outside the lock.
  Status status() {
    Status s;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    s.inProgress = state_.inProgress;
    s.hasResult = state_.hasResult;
    if (s.hasResult) s.result = result_;
    xSemaphoreGive(mutex_);
    return s;
  }

 private:
  SemaphoreHandle_t mutex_;
  BackgroundJobState state_;
  T result_{};
};
