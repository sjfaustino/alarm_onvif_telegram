#pragma once
#include <Arduino.h>
#include <PsychicHttp.h>
#include "telegram_users.h"
#include "background_job.h" // BackgroundJobStartOutcome

// Telegram Users panel: user list, Add/Edit form (permissions, camera
// subscriptions). Split out of webserver.cpp - see webserver_network.h's
// comment for why.

// prefill/isEdit repopulate the form after an edit link or a failed save -
// null prefill is the blank "Add Telegram user" state.
String renderUsersPanel(const TelegramUser* prefill, bool isEdit);

// PsychicRequest can't enumerate "all values for a repeated param name", so
// each camera gets its own checkbox ("cam_<name>") - probed by name.
TelegramUser parseUserForm(PsychicRequest* request);

// originalName is "" for a brand-new user (add), non-empty for an edit (the
// name the user had before this submission - user.name may differ, which
// is a rename).
bool saveUserSubmission(const TelegramUser& user, const String& originalName, String& banner);

// ============================================================
// Test message - see webserver_cameras.h's startTestAllCamerasAsync for
// why this can't run synchronously on the calling (PsychicHttp) task:
// sendTelegramMessage fans out to every systemMessages recipient, each
// capable of a 45s g_telegramNetMutex wait (telegram.cpp) - with more than
// one recipient configured, that's long enough to make the whole dashboard
// unreachable for everyone, not just whoever clicked the button, the same
// class of risk the Cameras page's "Test all"/"Search network" buttons
// already run as background tasks to avoid.
// ============================================================

// Starts sendTestMessage() on a background FreeRTOS task instead of the
// calling task. A no-op (doesn't start a second overlapping run) if one is
// already in progress - the return value tells the caller which of the
// three outcomes happened, for the /users/test route handler to show an
// accurate banner instead of always assuming success.
BackgroundJobStartOutcome startTestMessageAsync();

// Renders the current test-message status: "sending in the background"
// while one is in progress, the last completed run's result once one
// exists, or "" if no test has ever run this boot. Safe to call from any
// task (internally locked) - renderUsersPanel calls this itself, so it
// shows up on a normal page load too, not just right after clicking the
// button.
String renderTestMessageStatus();

// True while the test message job above is running - lets renderShell()
// (webserver.cpp) decide whether to auto-refresh the Users page instead of
// leaving the user to manually reload.
bool userJobsInProgress();
