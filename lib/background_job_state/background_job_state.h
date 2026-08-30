#pragma once

// Pure state machine behind BackgroundJob<T> (include/background_job.h) -
// split out so the actual decision logic (should a start request begin a
// new run, or is one already in flight) can be unit-tested natively
// (test/test_background_job_state) without FreeRTOS, which only exists
// on-device.
//
// This struct is NOT thread-safe on its own - BackgroundJob<T> only ever
// touches it while holding its own mutex. Kept free of any FreeRTOS/
// Arduino include so it stays testable the same way xml_helpers/
// camera_parse/onvif_discovery are (see test/README.md).
//
// Extracted after webserver_cameras.cpp ended up with two hand-written
// copies of this exact bool/bool/mutex shape (the "Test all cameras" and
// "Search network for cameras" buttons) - same rules, independently
// typed out twice, with neither copy's transition logic covered by a
// test. One implementation now, actually tested, used by both.
struct BackgroundJobState {
  bool inProgress = false;
  bool hasResult = false;
};

// Whether a start request should actually begin a new run. False means
// "one's already in flight, treat this as a no-op" - the caller must not
// spawn a second overlapping task in that case. A pure query, not a
// mutation - see markBackgroundJobStarted below for the actual transition,
// kept separate so a caller can decide what to do with the answer (spawn
// a task, or not) before committing to the state change.
bool shouldStartBackgroundJob(const BackgroundJobState& state);

// Transitions state after a start request shouldStartBackgroundJob said
// yes to. Deliberately does NOT touch hasResult - a job starting again
// doesn't erase the previous run's results, it just means renderers should
// prefer showing "running" over them (BackgroundJob<T>::status() reports
// inProgress and hasResult separately for exactly this reason - the
// previous results are still real, they're just stale as of the new run).
BackgroundJobState markBackgroundJobStarted(BackgroundJobState state);

// Transitions state after the background task completes.
BackgroundJobState markBackgroundJobFinished(BackgroundJobState state);

// Transitions state back after a start request markBackgroundJobStarted
// already committed to, but the caller then failed to actually launch the
// task (e.g. xTaskCreate returned pdFAIL, out of memory) - without this,
// inProgress would stay true forever, since nothing will ever call
// markBackgroundJobFinished for a task that never started, permanently
// wedging every future start request as a no-op until reboot. Real
// incident class this guards against: the one moment memory is tight
// enough for task creation to fail is often the same moment someone's
// trying to use a diagnostic feature (Test Connection, a storage check)
// to find out why. Deliberately does NOT touch hasResult - a failed START
// isn't a completed run, so any earlier real result stays visible until
// the next run actually finishes.
BackgroundJobState markBackgroundJobStartFailed(BackgroundJobState state);
