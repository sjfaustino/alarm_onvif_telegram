#include "telegram_users.h"
#include "telegram_user_serialize.h"
#include "nvs_chunk.h"
#include "secrets.h"
#include <Preferences.h>

static const char* NVS_NAMESPACE  = "tgusers";
// Legacy single-key format - see camera_store.cpp's identical
// NVS_KEY_LIST_LEGACY comment for why this is now chunked instead.
static const char* NVS_KEY_LIST_LEGACY = "list";
static const char* NVS_KEY_LIST_CHUNKS = "listChunks"; // uint16_t chunk count
static const size_t NVS_CHUNK_MAX_BYTES = 1500;
static const char* NVS_KEY_SCHEMA = "schema"; // see telegram_user_serialize.h's *_SCHEMA_VERSION comment

// Separates whole user records within the NVS blob (distinct from
// telegram_user_serialize.cpp's own FIELD_SEP/LIST_SEP, private to that
// file - this file only ever joins/splits on RECORD_SEP).
static const char RECORD_SEP = '\x1E';

static String chunkKey(uint16_t index) {
  char key[16];
  snprintf(key, sizeof(key), "list%u", (unsigned)index);
  return String(key);
}

std::vector<TelegramUser> loadTelegramUsers() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true); // read-only
  bool hasChunkedList = prefs.isKey(NVS_KEY_LIST_CHUNKS);
  bool hasLegacyList  = prefs.isKey(NVS_KEY_LIST_LEGACY);
  bool alreadyInitialized = hasChunkedList || hasLegacyList;

  String blob;
  if (hasChunkedList) {
    uint16_t chunkCount = prefs.getUShort(NVS_KEY_LIST_CHUNKS, 0);
    std::vector<String> chunks;
    chunks.reserve(chunkCount);
    for (uint16_t i = 0; i < chunkCount; i++) chunks.push_back(prefs.getString(chunkKey(i).c_str(), ""));
    blob = joinChunks(chunks);
  } else if (hasLegacyList) {
    blob = prefs.getString(NVS_KEY_LIST_LEGACY, "");
  }
  // 0 = written before schema versioning existed.
  uint16_t storedVersion = prefs.getUShort(NVS_KEY_SCHEMA, 0);
  prefs.end();

  if (!alreadyInitialized) {
    std::vector<TelegramUser> users;
    TelegramUser admin;
    admin.name = "Admin";
    admin.chatId = TELEGRAM_CHAT_ID;
    admin.allCameras = true;
    admin.systemMessages = true;
    admin.canCommand = true;
    admin.canSnap = true;
    users.push_back(admin);
    saveTelegramUsers(users);
    Serial.println("[telegram_users] First boot with NVS-backed user storage - seeded one "
                    "\"Admin\" user from secrets.h's TELEGRAM_CHAT_ID (all cameras, system "
                    "messages, can command, can snap).");
    return users;
  }

  if (storedVersion > TELEGRAM_USER_SCHEMA_VERSION) {
    Serial.printf("[telegram_users] WARNING: stored user schema (%u) is newer than this firmware "
                  "understands (%u) - was this board previously running newer firmware? Parsing "
                  "with the newest layout this build knows; some fields may come back wrong.\n",
                  (unsigned)storedVersion, (unsigned)TELEGRAM_USER_SCHEMA_VERSION);
  }

  std::vector<TelegramUser> users;
  int totalRecords = 0;
  int droppedRecords = 0;
  int recStart = 0;
  for (int i = 0; i <= (int)blob.length(); i++) {
    if (i == (int)blob.length() || blob[i] == RECORD_SEP) {
      if (i > recStart) {
        totalRecords++;
        String record = blob.substring(recStart, i);
        TelegramUser u = deserializeUser(record, storedVersion);
        if (u.name.length() > 0) {
          users.push_back(u);
        } else {
          droppedRecords++;
          Serial.printf("[telegram_users] WARNING: dropped a Telegram user record that failed to "
                        "parse under schema %u (found %u field(s)).\n", (unsigned)storedVersion,
                        (unsigned)telegramUserRecordFieldCount(record));
        }
      }
      recStart = i + 1;
    }
  }

  // One-time migration, same reasoning as camera_store.cpp's loadCameras() -
  // skipped if anything was dropped, so a parse failure can't turn into a
  // permanent NVS overwrite that deletes the unparsed records for good.
  if (droppedRecords > 0) {
    Serial.printf("[telegram_users] %d of %d user record(s) failed to parse - NOT migrating/rewriting "
                  "NVS this boot so the raw data isn't lost. Only the %u that parsed are active for "
                  "now; saving anything from this dashboard will overwrite the stored list, including "
                  "the unparsed records.\n", droppedRecords, totalRecords, (unsigned)users.size());
  } else if (storedVersion != TELEGRAM_USER_SCHEMA_VERSION) {
    Serial.printf("[telegram_users] Migrating %u user record(s) from schema %u to %u.\n",
                  (unsigned)users.size(), (unsigned)storedVersion, (unsigned)TELEGRAM_USER_SCHEMA_VERSION);
    saveTelegramUsers(users);
  }

  return users;
}

bool saveTelegramUsers(const std::vector<TelegramUser>& users) {
  String blob;
  for (size_t i = 0; i < users.size(); i++) {
    if (i > 0) blob += RECORD_SEP;
    blob += serializeUser(users[i]);
  }
  std::vector<String> chunks = splitIntoChunks(blob, NVS_CHUNK_MAX_BYTES);

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;

  // See camera_store.cpp's saveCameras() for why these return values are
  // checked - an unnoticed write failure here used to look identical to a
  // successful save.
  bool chunksOk = true;
  for (size_t i = 0; i < chunks.size(); i++) {
    if (prefs.putString(chunkKey((uint16_t)i).c_str(), chunks[i]) == 0) chunksOk = false;
  }
  uint16_t oldChunkCount = prefs.getUShort(NVS_KEY_LIST_CHUNKS, 0);
  for (uint16_t i = (uint16_t)chunks.size(); i < oldChunkCount; i++) prefs.remove(chunkKey(i).c_str());
  if (prefs.isKey(NVS_KEY_LIST_LEGACY)) prefs.remove(NVS_KEY_LIST_LEGACY);

  bool countOk = prefs.putUShort(NVS_KEY_LIST_CHUNKS, (uint16_t)chunks.size()) > 0;
  bool schemaOk = prefs.putUShort(NVS_KEY_SCHEMA, TELEGRAM_USER_SCHEMA_VERSION) > 0;
  prefs.end();

  if (!chunksOk || !countOk || !schemaOk) {
    Serial.printf("[telegram_users] ERROR: saveTelegramUsers failed to persist %u user(s) across %u "
                  "chunk(s), %u bytes total - NVS may be full. This WILL be lost on reboot.\n",
                  (unsigned)users.size(), (unsigned)chunks.size(), (unsigned)blob.length());
    return false;
  }
  return true;
}

bool addTelegramUser(const TelegramUser& user) {
  std::vector<TelegramUser> users = loadTelegramUsers();
  for (auto& u : users) {
    if (u.name.equalsIgnoreCase(user.name)) return false;
  }
  users.push_back(user);
  return saveTelegramUsers(users);
}

bool deleteTelegramUser(const String& name) {
  std::vector<TelegramUser> users = loadTelegramUsers();
  for (size_t i = 0; i < users.size(); i++) {
    if (users[i].name.equalsIgnoreCase(name)) {
      users.erase(users.begin() + i);
      return saveTelegramUsers(users);
    }
  }
  return false;
}

bool updateTelegramUser(const String& originalName, const TelegramUser& user) {
  std::vector<TelegramUser> users = loadTelegramUsers();
  int idx = -1;
  for (size_t i = 0; i < users.size(); i++) {
    if (users[i].name.equalsIgnoreCase(originalName)) { idx = (int)i; break; }
  }
  if (idx < 0) return false;

  for (size_t i = 0; i < users.size(); i++) {
    if ((int)i != idx && users[i].name.equalsIgnoreCase(user.name)) return false;
  }

  users[idx] = user;
  return saveTelegramUsers(users);
}
