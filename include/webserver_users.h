#pragma once
#include <Arduino.h>
#include <PsychicHttp.h>
#include "telegram_users.h"

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
