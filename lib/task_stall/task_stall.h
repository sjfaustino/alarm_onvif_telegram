#pragma once

// Pure alert-once/re-arm logic behind checkCameraTasks, tested natively.
struct TaskStallResult {
  bool shouldAlert = false;   // just crossed the threshold
  bool recovered = false;     // was alerted, and the task is beating again
  bool alerted = false;       // the caller's new "already alerted" state
};

// A task that isn't running (exited, never started) is never a stall.
// msSinceBeat is computed by the caller.
TaskStallResult evaluateTaskStall(bool taskRunning, unsigned long msSinceBeat, unsigned long thresholdMs,
                                  bool alreadyAlerted);
