# Native unit tests

```sh
pio test -e native
```

Compiles and runs on your own machine (or CI's runner) directly - no ESP32
hardware involved, no upload, no serial monitor. Each `test/test_*/` directory
is one Unity test binary; `pio test -e native` builds and runs all of them.

## What's covered, and why only this

This project is almost entirely I/O: ONVIF SOAP calls over the network,
NVS reads/writes, WiFi/TLS/HTTP, FreeRTOS tasks, a web server. None of that
can be unit-tested without either real hardware, a full ESP32 emulator, or
enough mocking that the tests would mostly be exercising the mocks instead
of the code. That's not a gap this test suite tries to paper over.

What *is* covered is the pure logic that used to be tangled up inside those
I/O-heavy files - extracted into `lib/*` modules (no `WiFi.h`/`HTTPClient.h`/
`Preferences.h`/`PsychicHttp.h` in sight) specifically so it could be tested
this way:

| Module (`lib/`)             | Extracted from                         | Covers |
|---|---|---|
| `xml_helpers`                | `onvif_soap.cpp`                       | ONVIF response substring parsing (`findElementByLocalName`, `findAttributeValue`/`findAttributeInTag`, `responseHasFault`) and `xmlEscape` - hardened against inconsistent attribute quoting and a closing tag that drops its namespace prefix |
| `camera_serialize`           | `camera_store.cpp`                     | `CameraConfig` <-> NVS blob (de)serialization, schema-versioned (see below) |
| `telegram_user_serialize`    | `telegram_users.cpp`                   | `TelegramUser` <-> NVS blob (de)serialization (also schema-versioned), and `telegramUserWantsCamera` |
| `telegram_parse`             | `telegram.cpp`                         | `parseTelegramUpdates` (ArduinoJson, replacing hand-rolled brace-counting) and the `/on`/`/off`/`/snap` camera-name prefix matching |
| `backoff`                    | `main.cpp` + `camera.cpp` (duplicated) | The doubling-with-a-cap retry delay formula, previously hand-written twice and prone to drifting apart |

### Why the serialization modules are schema-versioned

`camera_serialize`/`telegram_user_serialize` each store a `CAMERA_SCHEMA_VERSION`/
`TELEGRAM_USER_SCHEMA_VERSION` alongside their NVS blob (`camera_store.cpp`/
`telegram_users.cpp` read+write a separate `"schema"` key next to `"list"`).
This exists to close a real gap: a purely positional, pipe-delimited format
can't tell "an old record that's missing some trailing fields" apart from
"a record whose fields just moved" - both just look like a different field
count. Before versioning, deserializing leaned on `fields.size()` tolerance
to handle the former case, which happened to be safe *only* because every
historical field change in this project was, in fact, a pure append. That
was an assumption sitting on the honor system, not something enforced -
nothing would have caught a future field getting inserted in the middle of
the layout instead of appended at the end, and the result would have been
every existing saved camera/user silently misparsing into the wrong values
on the next boot, with no crash and no error.

Versioning turns that from an assumption into a rule: the *current* schema
version's parser now requires an *exact* field count (a mismatch is treated
as corruption, not guessed at), and any future layout change must add a new
explicitly-numbered version branch rather than editing the current one in
place. Old data (schema 0 - "written before this existed") still parses via
the original tolerant logic, and gets transparently rewritten in the
current format + version the next time it's loaded. `test_v1_wrong_field_count_is_rejected_not_reinterpreted`
in both test files is what demonstrates the fix directly: the exact same
malformed input that a pre-versioning build would have silently accepted
now comes back empty instead.

## What's *not* covered, and what it would take

- **`onvif_soap.cpp`'s `soapPost`/`makeSecurityHeader`/`isoTimeNow`, `camera.cpp`,
  `telegram.cpp`'s send paths, `webserver.cpp`, `main.cpp`'s boot sequence** -
  all fundamentally about talking to a network/clock/NVS that doesn't exist
  on a CI runner. Testing these for real would mean either running against
  actual ONVIF cameras and a real Telegram bot (not reproducible, not CI-safe),
  or building a fake HTTPClient/Preferences/WiFi layer detailed enough to be
  a second implementation worth its own tests.
- **The FreeRTOS task/concurrency behavior** (per-camera tasks, the task
  watchdog, the web server's request handling) - needs either real hardware
  or a FreeRTOS simulator; out of scope here.
- **`webserver.cpp`'s ~1000 lines of hand-built HTML** - not logic so much
  as string assembly; the highest-value thing to test there would be that
  every user-controlled value passed to `htmlEscape()` actually gets there,
  which is more of a lint/audit than a unit test. Not attempted yet.

## Adding a new pure-logic test

1. If the logic isn't already separated from its I/O-coupled caller, pull it
   into a new (or existing) `lib/<name>/` module - `.h`/`.cpp`, no
   `Preferences.h`/`WiFi.h`/etc. If it uses `String`, `#include <Arduino.h>`
   directly in the module's own header, even if it'd also arrive transitively
   through another project header - PlatformIO's dependency finder resolves
   `env:native`'s `ArduinoFake` dependency by scanning each `lib/` module's
   *own* includes, not through an indirect chain into `include/`.
2. Have the original `src/*.cpp` file `#include` the new module's header and
   call into it instead of keeping its own copy.
3. Add `test/test_<name>/test_<name>.cpp` (Unity - see any existing one for
   the pattern) and `#include` the new module's header.
4. `pio test -e native`, then `pio run -e esp32s3` to confirm the extraction
   didn't change the firmware build.
