#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "background_job_state.h"

// Single-slot background job: mutex-guarded start / finish / status around
// BackgroundJobState's pure rules.
//
// Start outcome, so the route handler's banner tells the truth (including a
// failed task launch).
enum class BackgroundJobStartOutcome {
  Started,        // tryStart() and the task creation that followed both succeeded - a new run is in flight
  AlreadyRunning, // tryStart() returned false - one was already in flight, this call changed nothing
  FailedToStart,  // xTaskCreate failed (out of memory?) after tryStart() succeeded; rolled back via cancelStart()
};

// T is the job's result type; must be default-constructible.
template <typename T>
class BackgroundJob {
 public:
  BackgroundJob() : mutex_(xSemaphoreCreateMutex()) {}

  // False if a run is in progress - don't start another. True: the caller must
  // start a task that eventually calls finish().
  bool tryStart() {
    bool started;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    started = shouldStartBackgroundJob(state_);
    if (started) state_ = markBackgroundJobStarted(state_);
    xSemaphoreGive(mutex_);
    return started;
  }

  // Undo tryStart() when the task failed to launch, so the job isn't wedged.
  void cancelStart() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    state_ = markBackgroundJobStartFailed(state_);
    xSemaphoreGive(mutex_);
  }

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

  // Any task. The result is copied out so the caller renders outside the lock.
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
