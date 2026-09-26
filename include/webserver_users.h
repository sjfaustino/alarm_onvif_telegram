#pragma once
#include <Arduino.h>
#include <PsychicHttp.h>
#include "telegram_users.h"
#include "background_job.h" // BackgroundJobStartOutcome

// Telegram Users panel: user list and Add/Edit form.

// prefill/isEdit repopulate the form (null = blank Add form).
String renderUsersPanel(const TelegramUser* prefill, bool isEdit);

// PsychicRequest can't list repeated params, so each camera has its own
// "cam_<name>" checkbox.
TelegramUser parseUserForm(PsychicRequest* request);

// originalName is "" for a new user; user.name may differ on a rename.
bool saveUserSubmission(const TelegramUser& user, const String& originalName, String& banner);

// ============================================================
// Test message. Fans out to every system-message recipient, each able to wait
// 45s for the Telegram mutex, so it runs on a background task.
// ============================================================

// Starts sendTestMessage on a task; reports if one is already running.
BackgroundJobStartOutcome startTestMessageAsync();

// Test status HTML: running, last result, or "".
String renderTestMessageStatus();

// True while the job runs, so the page auto-refreshes.
bool userJobsInProgress();
