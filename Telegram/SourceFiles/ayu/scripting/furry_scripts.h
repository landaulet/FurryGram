// FurryGram scripting engine (QuickJS). Loads user .js plugins from a `scripts/`
// folder next to the executable and exposes a small `furry` API to them:
//   furry.log(...)                 -> write to the app log
//   furry.on('message', cb)        -> cb({ chatId, text, out, senderId }) on new messages
//   furry.sendMessage(chatId, txt) -> send a text message to a chat
//   furry.version                  -> string
#pragma once

#include <QtCore/QString>
#include <vector>
#include <rpl/producer.h>

namespace FurryScripts {

struct ScriptInfo {
	QString name; // file name, e.g. "hello.js"
	bool enabled = true; // not in scripts/.disabled
	bool ok = true; // loaded without a JS exception (only meaningful if enabled)
	QString error; // error text if !ok
	bool hasSettings = false; // script called furry.registerSettings(...)
};

// One declared setting (via furry.registerSettings). type: "toggle"|"number"|"text".
struct ScriptOption {
	QString key;
	QString type;
	QString label;
	QString defString; // default for text
	double defNumber = 0; // default for number
	bool defBool = false; // default for toggle
};

struct ScriptSettingsSchema {
	QString title; // section title
	std::vector<ScriptOption> options;
};

// Schema a script declared (empty if it declared none). `name` = file name.
[[nodiscard]] ScriptSettingsSchema ScriptSchema(const QString &name);

// Persisted per-script setting values (scripts/<name>.settings.json).
[[nodiscard]] bool GetScriptBool(const QString &name, const QString &key, bool fallback);
[[nodiscard]] double GetScriptNumber(const QString &name, const QString &key, double fallback);
[[nodiscard]] QString GetScriptText(const QString &name, const QString &key, const QString &fallback);
void SetScriptBool(const QString &name, const QString &key, bool value);
void SetScriptNumber(const QString &name, const QString &key, double value);
void SetScriptText(const QString &name, const QString &key, const QString &value);

// Initialize the JS runtime, expose the API and load all scripts. Call once at
// startup (main thread). Safe to call before login; the message bridge attaches
// to whichever account becomes active.
void Start();

// Unload all scripts, reset the JS context and re-load from disk. For the
// settings "Reload" button.
void Reload();

// Snapshot of currently loaded scripts (for the settings list).
[[nodiscard]] std::vector<ScriptInfo> LoadedScripts();

// Enable/disable a script by file name (persists to scripts/.disabled and reloads).
void SetScriptEnabled(const QString &name, bool enabled);

// Is a script currently enabled? (false if unknown.)
[[nodiscard]] bool IsScriptEnabled(const QString &name);

// Fires whenever the loaded-scripts set or their enabled state changes.
[[nodiscard]] rpl::producer<> ScriptsChanged();

// Absolute path of the scripts folder (created if missing).
[[nodiscard]] QString ScriptsFolder();

} // namespace FurryScripts
