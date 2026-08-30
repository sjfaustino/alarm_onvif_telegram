#include "background_job_state.h"

bool shouldStartBackgroundJob(const BackgroundJobState& state) {
  return !state.inProgress;
}

BackgroundJobState markBackgroundJobStarted(BackgroundJobState state) {
  state.inProgress = true;
  return state;
}

BackgroundJobState markBackgroundJobFinished(BackgroundJobState state) {
  state.inProgress = false;
  state.hasResult = true;
  return state;
}

BackgroundJobState markBackgroundJobStartFailed(BackgroundJobState state) {
  state.inProgress = false;
  return state;
}
