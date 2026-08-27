#pragma once
#include <Arduino.h> // explicit, not chained - see camera_serialize.h's comment

// Small pure HTML-snippet builders shared across the dashboard's list
// panels (Cameras, Telegram Users) - split out because webserver_cameras.cpp
// and webserver_users.cpp each had their own byte-identical copy of this
// exact block, differing only in the routes and the item's name.

// The "Edit | Delete" actions cell at the end of a list table row - an
// edit link plus a POST form with a JS confirm() dialog, both keyed by
// itemName. editRouteBase is the edit link's URL up to (not including) the
// name itself, e.g. "/cameras/edit?name=" - itemName is percent-encoded
// and appended to it. deleteRoute is the delete form's action, used as-is
// (the two current callers don't actually share a naming convention here -
// cameras delete via plain "/delete", users via "/users/delete" - so this
// takes the exact route rather than assuming one).
String renderEditDeleteActions(const String& editRouteBase, const String& deleteRoute, const String& itemName);
