#include "task_stall.h"

TaskStallResult evaluateTaskStall(bool taskRunning, unsigned long msSinceBeat, unsigned long thresholdMs,
                                  bool alreadyAlerted) {
  TaskStallResult r;
  bool stalled = taskRunning && msSinceBeat >= thresholdMs;
  r.shouldAlert = stalled && !alreadyAlerted;
  r.recovered = !stalled && alreadyAlerted;
  r.alerted = stalled;
  return r;
}
