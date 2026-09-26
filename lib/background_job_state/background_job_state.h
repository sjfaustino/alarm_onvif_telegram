#pragma once

// Pure state machine behind BackgroundJob<T>, tested natively. Not thread-safe
// by itself; BackgroundJob<T> holds its mutex around every use.
struct BackgroundJobState {
  bool inProgress = false;
  bool hasResult = false;
};

// False = a run is already in flight; don't start another. Query only.
bool shouldStartBackgroundJob(const BackgroundJobState& state);

// Keeps hasResult: the previous result stays visible (stale) while running.
BackgroundJobState markBackgroundJobStarted(BackgroundJobState state);

BackgroundJobState markBackgroundJobFinished(BackgroundJobState state);

// Undo for a start whose task failed to launch (e.g. out of memory); otherwise
// inProgress would stick until reboot, just when diagnostics are needed. Keeps
// hasResult.
BackgroundJobState markBackgroundJobStartFailed(BackgroundJobState state);
