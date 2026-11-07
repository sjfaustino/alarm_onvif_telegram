#pragma once

// Guards the /debug routes. An empty configured token means they're off. The
// comparison time doesn't depend on where the strings differ.
bool debugTokenValid(const char* configured, const char* supplied);
