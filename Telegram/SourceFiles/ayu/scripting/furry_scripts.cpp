// FurryGram scripting engine (QuickJS).
#include "ayu/scripting/furry_scripts.h"

#include "base/debug_log.h"
#include "core/application.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "apiwrap.h"
#include "api/api_common.h"
#include "api/api_editing.h"
#include "data/data_session.h"
#include "data/data_changes.h"
#include "data/data_peer.h"
#include "history/history.h"
#include "history/history_item.h"
#include "ui/text/text_entity.h"

#include <map>
#include <set>
#include <vector>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QRegularExpression>
#include <QtGui/QGuiApplication>

#include "quickjs.h"

namespace FurryScripts {
namespace {

class Manager final {
public:
	Manager();
	~Manager();

	void loadScripts();
	void reload();
	void attachSession(Main::Session *session);
	void dispatchMessage(not_null<HistoryItem*> item);

	JSContext *ctx() const {
		return _ctx;
	}
	void addMessageHandler(JSValue fn);
	void addDeletedHandler(JSValue fn);
	void doSendMessage(std::int64_t chatId, const QString &text);
	void doEditMessage(std::int64_t chatId, std::int64_t msgId, const QString &text);
	void doWriteFile(const QString &name, const QString &text);
	void dispatchDeleted(not_null<HistoryItem*> item);
	[[nodiscard]] std::int64_t savedMessagesId() const {
		return _session ? std::int64_t(_session->userPeerId().value) : 0;
	}

	[[nodiscard]] std::vector<ScriptInfo> scripts() const {
		return _scripts;
	}
	[[nodiscard]] static QString folder();
	void setEnabled(const QString &name, bool enabled);
	[[nodiscard]] bool isEnabled(const QString &name) const {
		return ranges::find(_disabled, name) == end(_disabled);
	}

	// Settings schema/values (Stage B).
	void registerSchema(const QString &id, ScriptSettingsSchema schema);
	void mapId(const QString &id, const QString &file) {
		if (!id.isEmpty() && !file.isEmpty()) {
			_idToFile[id] = file;
		}
	}
	[[nodiscard]] ScriptSettingsSchema schema(const QString &name) const;
	[[nodiscard]] bool schemaOnly() const { return _schemaOnly; }

	// The script currently being evaluated (so registerSettings knows its file).
	QString _loadingScript;
	// When evaluating a DISABLED script we still run it to collect its settings
	// schema, but suppress active effects (on/sendMessage) so it stays inert.
	bool _schemaOnly = false;

private:
	void createContext();
	void destroyContext();
	void evalFile(const QString &path, bool enabled);
	QString takeException();
	void loadDisabled();
	void saveDisabled();

	std::vector<QString> _disabled;
	std::map<QString, ScriptSettingsSchema> _schemas; // by file name
	std::map<QString, QString> _idToFile; // registered id -> file name

public:
	[[nodiscard]] QString fileForId(const QString &id) const {
		const auto it = _idToFile.find(id);
		return (it != _idToFile.end()) ? it->second : id;
	}
private:

	JSRuntime *_rt = nullptr;
	JSContext *_ctx = nullptr;
	std::vector<JSValue> _messageHandlers;
	std::vector<JSValue> _deletedHandlers;
	std::vector<ScriptInfo> _scripts;
	std::set<std::pair<std::uint64_t, std::int64_t>> _seenMessages; // dedup (peer,msg)
	rpl::lifetime _sessionLifetime;
	rpl::lifetime _accountLifetime;
	Main::Session *_session = nullptr;

};

Manager *GlobalManager = nullptr;
rpl::event_stream<> ChangedStream;

[[nodiscard]] Manager *Self(JSContext *ctx) {
	return static_cast<Manager*>(JS_GetContextOpaque(ctx));
}

// ---- native API functions exposed to JS ----

JSValue Api_log(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	auto parts = QStringList();
	for (auto i = 0; i != argc; ++i) {
		const auto s = JS_ToCString(ctx, argv[i]);
		if (s) {
			parts.push_back(QString::fromUtf8(s));
			JS_FreeCString(ctx, s);
		}
	}
	LOG(("[furry-script] %1").arg(parts.join(' ')));
	return JS_UNDEFINED;
}

JSValue Api_on(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	if (argc < 2) {
		return JS_UNDEFINED;
	}
	const auto event = JS_ToCString(ctx, argv[0]);
	const auto name = event ? QString::fromUtf8(event) : QString();
	if (event) {
		JS_FreeCString(ctx, event);
	}
	if (!JS_IsFunction(ctx, argv[1])) {
		return JS_UNDEFINED;
	}
	const auto self = Self(ctx);
	if (!self || self->schemaOnly()) {
		return JS_UNDEFINED;
	}
	if (name == u"message"_q) {
		self->addMessageHandler(JS_DupValue(ctx, argv[1]));
	} else if (name == u"messageDeleted"_q) {
		self->addDeletedHandler(JS_DupValue(ctx, argv[1]));
	}
	return JS_UNDEFINED;
}

JSValue Api_sendMessage(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	if (argc < 2) {
		return JS_UNDEFINED;
	}
	std::int64_t chatId = 0;
	JS_ToInt64(ctx, &chatId, argv[0]);
	const auto text = JS_ToCString(ctx, argv[1]);
	const auto qtext = text ? QString::fromUtf8(text) : QString();
	if (text) {
		JS_FreeCString(ctx, text);
	}
	if (!qtext.isEmpty()) {
		if (const auto self = Self(ctx); self && !self->schemaOnly()) {
			self->doSendMessage(chatId, qtext);
		}
	}
	return JS_UNDEFINED;
}

// savedMessagesId() -> chatId of your own "Saved Messages" (for self-alerts).
JSValue Api_savedMessagesId(JSContext *ctx, JSValueConst, int, JSValueConst *) {
	const auto self = Self(ctx);
	return JS_NewInt64(ctx, self ? self->savedMessagesId() : 0);
}

// editMessage(chatId, msgId, newText) — edit an existing message (markdown-aware).
JSValue Api_editMessage(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	if (argc < 3) {
		return JS_UNDEFINED;
	}
	std::int64_t chatId = 0, msgId = 0;
	JS_ToInt64(ctx, &chatId, argv[0]);
	JS_ToInt64(ctx, &msgId, argv[1]);
	const auto t = JS_ToCString(ctx, argv[2]);
	const auto text = t ? QString::fromUtf8(t) : QString();
	if (t) {
		JS_FreeCString(ctx, t);
	}
	if (const auto self = Self(ctx); self && !self->schemaOnly()) {
		self->doEditMessage(chatId, msgId, text);
	}
	return JS_UNDEFINED;
}

// writeFile(name, text) — append a line to scripts/<name> (sandboxed to folder).
JSValue Api_writeFile(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	if (argc < 2) {
		return JS_UNDEFINED;
	}
	const auto n = JS_ToCString(ctx, argv[0]);
	const auto name = n ? QString::fromUtf8(n) : QString();
	if (n) {
		JS_FreeCString(ctx, n);
	}
	const auto t = JS_ToCString(ctx, argv[1]);
	const auto text = t ? QString::fromUtf8(t) : QString();
	if (t) {
		JS_FreeCString(ctx, t);
	}
	if (const auto self = Self(ctx); self && !self->schemaOnly()) {
		self->doWriteFile(name, text);
	}
	return JS_UNDEFINED;
}

QString Manager::folder() {
	// Store scripts in the working/data dir (next to tdata) so they persist
	// regardless of the exe location. For installed builds applicationDirPath()
	// would be Program Files (read-only); cWorkingDir() is always writable.
	return cWorkingDir() + u"scripts"_q;
}

// ---- per-script settings storage (scripts/<name>.settings.json) ----

QString SettingsPath(const QString &name) {
	return Manager::folder() + u"/"_q + name + u".settings.json"_q;
}

QJsonObject ReadValues(const QString &name) {
	auto file = QFile(SettingsPath(name));
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	const auto data = file.readAll();
	file.close();
	return QJsonDocument::fromJson(data).object();
}

void WriteValues(const QString &name, const QJsonObject &obj) {
	QDir().mkpath(Manager::folder());
	auto file = QFile(SettingsPath(name));
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return;
	}
	file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
	file.close();
}

// registerSettings(id, { title, options: [{key,type,label,default}] })
JSValue Api_registerSettings(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	const auto self = Self(ctx);
	if (!self || argc < 2 || !JS_IsObject(argv[1])) {
		return JS_UNDEFINED;
	}
	auto schema = ScriptSettingsSchema();

	auto titleVal = JS_GetPropertyStr(ctx, argv[1], "title");
	if (const auto s = JS_ToCString(ctx, titleVal)) {
		schema.title = QString::fromUtf8(s);
		JS_FreeCString(ctx, s);
	}
	JS_FreeValue(ctx, titleVal);

	auto optionsVal = JS_GetPropertyStr(ctx, argv[1], "options");
	if (JS_IsArray(optionsVal)) {
		auto lenVal = JS_GetPropertyStr(ctx, optionsVal, "length");
		int32_t len = 0;
		JS_ToInt32(ctx, &len, lenVal);
		JS_FreeValue(ctx, lenVal);
		const auto readStr = [&](JSValueConst o, const char *k) {
			auto v = JS_GetPropertyStr(ctx, o, k);
			auto out = QString();
			if (const auto s = JS_ToCString(ctx, v)) {
				out = QString::fromUtf8(s);
				JS_FreeCString(ctx, s);
			}
			JS_FreeValue(ctx, v);
			return out;
		};
		for (auto i = 0; i < len; ++i) {
			auto o = JS_GetPropertyUint32(ctx, optionsVal, i);
			if (JS_IsObject(o)) {
				auto opt = ScriptOption();
				opt.key = readStr(o, "key");
				opt.type = readStr(o, "type");
				opt.label = readStr(o, "label");
				auto def = JS_GetPropertyStr(ctx, o, "default");
				if (opt.type == u"toggle"_q) {
					opt.defBool = JS_ToBool(ctx, def) > 0;
				} else if (opt.type == u"number"_q) {
					JS_ToFloat64(ctx, &opt.defNumber, def);
				} else {
					if (const auto s = JS_ToCString(ctx, def)) {
						opt.defString = QString::fromUtf8(s);
						JS_FreeCString(ctx, s);
					}
				}
				JS_FreeValue(ctx, def);
				if (!opt.key.isEmpty()) {
					schema.options.push_back(opt);
				}
			}
			JS_FreeValue(ctx, o);
		}
	}
	JS_FreeValue(ctx, optionsVal);

	// Map the registered id (argv[0]) to the current file, so furry.get(id,...)
	// from event callbacks can resolve back to the file (where the schema/values
	// live), since _loadingScript is empty outside initial eval.
	if (const auto idC = JS_ToCString(ctx, argv[0])) {
		self->mapId(QString::fromUtf8(idC), self->_loadingScript);
		JS_FreeCString(ctx, idC);
	}
	self->registerSchema(self->_loadingScript, std::move(schema));
	return JS_UNDEFINED;
}

// get(id, key) -> stored value (or declared default), typed by the schema.
JSValue Api_get(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	const auto self = Self(ctx);
	if (!self || argc < 2) {
		return JS_UNDEFINED;
	}
	// Values are keyed by the script's REGISTERED id (argv[0]), not _loadingScript:
	// _loadingScript is only valid during initial eval, but furry.get() is mostly
	// called later from event callbacks when it's empty.
	const auto idC = JS_ToCString(ctx, argv[0]);
	const auto id = idC ? QString::fromUtf8(idC) : QString();
	if (idC) {
		JS_FreeCString(ctx, idC);
	}
	const auto keyC = JS_ToCString(ctx, argv[1]);
	const auto key = keyC ? QString::fromUtf8(keyC) : QString();
	if (keyC) {
		JS_FreeCString(ctx, keyC);
	}
	const auto name = self->fileForId(id);
	const auto sc = self->schema(name);
	const ScriptOption *opt = nullptr;
	for (const auto &o : sc.options) {
		if (o.key == key) { opt = &o; break; }
	}
	if (!opt) {
		return JS_UNDEFINED;
	}
	if (opt->type == u"toggle"_q) {
		return JS_NewBool(ctx, GetScriptBool(name, key, opt->defBool) ? 1 : 0);
	} else if (opt->type == u"number"_q) {
		return JS_NewFloat64(ctx, GetScriptNumber(name, key, opt->defNumber));
	}
	const auto v = GetScriptText(name, key, opt->defString).toUtf8();
	return JS_NewString(ctx, v.constData());
}

Manager::Manager() {
	createContext();

	// Attach to the active account (and follow account switches).
	Core::App().domain().activeSessionValue(
	) | rpl::on_next([=](Main::Session *session) {
		attachSession(session);
	}, _accountLifetime);
}

Manager::~Manager() {
	destroyContext();
}

void Manager::createContext() {
	_rt = JS_NewRuntime();
	_ctx = JS_NewContext(_rt);
	JS_SetContextOpaque(_ctx, this);

	auto global = JS_GetGlobalObject(_ctx);
	auto furry = JS_NewObject(_ctx);
	JS_SetPropertyStr(_ctx, furry, "log",
		JS_NewCFunction(_ctx, Api_log, "log", 1));
	JS_SetPropertyStr(_ctx, furry, "on",
		JS_NewCFunction(_ctx, Api_on, "on", 2));
	JS_SetPropertyStr(_ctx, furry, "sendMessage",
		JS_NewCFunction(_ctx, Api_sendMessage, "sendMessage", 2));
	JS_SetPropertyStr(_ctx, furry, "registerSettings",
		JS_NewCFunction(_ctx, Api_registerSettings, "registerSettings", 2));
	JS_SetPropertyStr(_ctx, furry, "get",
		JS_NewCFunction(_ctx, Api_get, "get", 2));
	JS_SetPropertyStr(_ctx, furry, "savedMessagesId",
		JS_NewCFunction(_ctx, Api_savedMessagesId, "savedMessagesId", 0));
	JS_SetPropertyStr(_ctx, furry, "editMessage",
		JS_NewCFunction(_ctx, Api_editMessage, "editMessage", 3));
	JS_SetPropertyStr(_ctx, furry, "writeFile",
		JS_NewCFunction(_ctx, Api_writeFile, "writeFile", 2));
	JS_SetPropertyStr(_ctx, furry, "version",
		JS_NewString(_ctx, "FurryGram 0.1"));
	JS_SetPropertyStr(_ctx, global, "furry", furry);
	JS_FreeValue(_ctx, global);
}

void Manager::destroyContext() {
	for (auto &fn : _messageHandlers) {
		JS_FreeValue(_ctx, fn);
	}
	_messageHandlers.clear();
	for (auto &fn : _deletedHandlers) {
		JS_FreeValue(_ctx, fn);
	}
	_deletedHandlers.clear();
	if (_ctx) {
		JS_FreeContext(_ctx);
		_ctx = nullptr;
	}
	if (_rt) {
		JS_FreeRuntime(_rt);
		_rt = nullptr;
	}
}

void Manager::addMessageHandler(JSValue fn) {
	_messageHandlers.push_back(fn);
}

void Manager::addDeletedHandler(JSValue fn) {
	_deletedHandlers.push_back(fn);
}

QString Manager::takeException() {
	auto exc = JS_GetException(_ctx);
	auto result = QString();
	const auto s = JS_ToCString(_ctx, exc);
	if (s) {
		result = QString::fromUtf8(s);
		JS_FreeCString(_ctx, s);
	}
	JS_FreeValue(_ctx, exc);
	return result;
}

void Manager::evalFile(const QString &path, bool enabled) {
	auto info = ScriptInfo{ .name = QFileInfo(path).fileName(), .enabled = enabled };
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		info.ok = false;
		info.error = u"can't open file"_q;
		_scripts.push_back(info);
		return;
	}
	const auto bytes = file.readAll();
	file.close();
	const auto name = path.toUtf8();
	_loadingScript = info.name; // so furry.registerSettings/get know the file
	auto result = JS_Eval(
		_ctx,
		bytes.constData(),
		bytes.size(),
		name.constData(),
		JS_EVAL_TYPE_GLOBAL);
	if (JS_IsException(result)) {
		info.ok = false;
		info.error = takeException();
		LOG(("[furry-script] error in %1: %2").arg(info.name, info.error));
	} else {
		LOG(("[furry-script] loaded %1").arg(path));
	}
	JS_FreeValue(_ctx, result);
	_loadingScript = QString();
	info.hasSettings = (_schemas.find(info.name) != _schemas.end());
	_scripts.push_back(info);
}

void Manager::registerSchema(const QString &id, ScriptSettingsSchema schema) {
	if (!id.isEmpty()) {
		_schemas[id] = std::move(schema);
	}
}

ScriptSettingsSchema Manager::schema(const QString &name) const {
	const auto it = _schemas.find(name);
	return (it != _schemas.end()) ? it->second : ScriptSettingsSchema();
}

void Manager::loadDisabled() {
	_disabled.clear();
	auto file = QFile(folder() + u"/.disabled"_q);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return;
	}
	while (!file.atEnd()) {
		const auto line = QString::fromUtf8(file.readLine()).trimmed();
		if (!line.isEmpty()) {
			_disabled.push_back(line);
		}
	}
	file.close();
}

void Manager::saveDisabled() {
	const auto path = folder() + u"/.disabled"_q;
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		return;
	}
	for (const auto &name : _disabled) {
		file.write(name.toUtf8() + '\n');
	}
	file.close();
}

void Manager::loadScripts() {
	_scripts.clear();
	_schemas.clear();
	_idToFile.clear();
	loadDisabled();
	const auto dirPath = folder();
	auto dir = QDir(dirPath);
	if (!dir.exists()) {
		dir.mkpath(dirPath);
		LOG(("[furry-script] created scripts dir: %1").arg(dirPath));
		return;
	}
	const auto files = dir.entryList(QStringList{ u"*.js"_q }, QDir::Files, QDir::Name);
	LOG(("[furry-script] scanning %1: %2 file(s)").arg(dirPath).arg(files.size()));
	for (const auto &f : files) {
		// Always evaluate the file so its settings schema is collected. Disabled
		// scripts run in schema-only mode: registerSettings works, but on/sendMessage
		// are suppressed so they have no active effect.
		const auto disabled = (ranges::find(_disabled, f) != end(_disabled));
		_schemaOnly = disabled;
		evalFile(dir.filePath(f), !disabled);
		_schemaOnly = false;
	}
}

void Manager::setEnabled(const QString &name, bool enabled) {
	const auto it = ranges::find(_disabled, name);
	const auto currentlyDisabled = (it != end(_disabled));
	if (enabled && currentlyDisabled) {
		_disabled.erase(it);
	} else if (!enabled && !currentlyDisabled) {
		_disabled.push_back(name);
	} else {
		return; // no change
	}
	saveDisabled();
	ChangedStream.fire({}); // update list label immediately
	// Defer the heavy reload (recreates the JS context) to the next event-loop
	// tick: doing it synchronously inside the toggle handler swallowed the first
	// state change.
	crl::on_main([] {
		if (GlobalManager) {
			GlobalManager->reload();
		}
	});
}

void Manager::reload() {
	_sessionLifetime.destroy();
	destroyContext();
	createContext();
	loadScripts();
	attachSession(_session);
	LOG(("[furry-script] reloaded"));
}

void Manager::attachSession(Main::Session *session) {
	_sessionLifetime.destroy();
	_session = session;
	if (!session) {
		return;
	}
	session->changes().messageUpdates(
		Data::MessageUpdate::Flag::NewAdded
		| Data::MessageUpdate::Flag::NewMaybeAdded
	) | rpl::on_next([=](const Data::MessageUpdate &update) {
		dispatchMessage(update.item);
	}, _sessionLifetime);

	session->changes().messageUpdates(
		Data::MessageUpdate::Flag::Destroyed
	) | rpl::on_next([=](const Data::MessageUpdate &update) {
		dispatchDeleted(update.item);
	}, _sessionLifetime);
}

void Manager::dispatchDeleted(not_null<HistoryItem*> item) {
	if (_deletedHandlers.empty()) {
		return;
	}
	const auto chatId = std::int64_t(item->history()->peer->id.value);
	const auto messageId = std::int64_t(item->id.bare);
	const auto from = item->from();
	const auto senderId = std::int64_t(from->id.value);
	const auto senderName = from->name().toUtf8();
	const auto text = item->originalText().text.toUtf8();
	const auto out = item->out();

	auto obj = JS_NewObject(_ctx);
	JS_SetPropertyStr(_ctx, obj, "chatId", JS_NewInt64(_ctx, chatId));
	JS_SetPropertyStr(_ctx, obj, "messageId", JS_NewInt64(_ctx, messageId));
	JS_SetPropertyStr(_ctx, obj, "senderId", JS_NewInt64(_ctx, senderId));
	JS_SetPropertyStr(_ctx, obj, "senderName", JS_NewString(_ctx, senderName.constData()));
	JS_SetPropertyStr(_ctx, obj, "text", JS_NewString(_ctx, text.constData()));
	JS_SetPropertyStr(_ctx, obj, "out", JS_NewBool(_ctx, out ? 1 : 0));

	for (auto &fn : _deletedHandlers) {
		JSValueConst args[1] = { obj };
		auto r = JS_Call(_ctx, fn, JS_UNDEFINED, 1, args);
		if (JS_IsException(r)) {
			const auto err = takeException();
			LOG(("[furry-script] deleted handler error: %1").arg(err));
		}
		JS_FreeValue(_ctx, r);
	}
	JS_FreeValue(_ctx, obj);
}

void Manager::dispatchMessage(not_null<HistoryItem*> item) {
	if (_messageHandlers.empty()) {
		return;
	}
	// Dedup: NewAdded|NewMaybeAdded can fire MANY times for the same message (on
	// reads, edits, reactions...). Dispatch each (chat,msg) to JS only ONCE,
	// otherwise scripts that act on a message would fire repeatedly (spam loop).
	const auto peerValue = item->history()->peer->id.value;
	const auto msgBare = item->fullId().msg.bare;
	if (!_seenMessages.emplace(peerValue, msgBare).second) {
		return;
	}
	if (_seenMessages.size() > 4096) {
		_seenMessages.clear(); // simple bound; dedup is best-effort
	}
	const auto chatId = std::int64_t(item->history()->peer->id.value);
	const auto from = item->from();
	const auto senderId = std::int64_t(from->id.value);
	const auto senderName = from->name().toUtf8();
	const auto text = item->originalText().text.toUtf8();
	const auto out = item->out();

	const auto messageId = std::int64_t(item->id.bare);

	auto obj = JS_NewObject(_ctx);
	JS_SetPropertyStr(_ctx, obj, "chatId", JS_NewInt64(_ctx, chatId));
	JS_SetPropertyStr(_ctx, obj, "messageId", JS_NewInt64(_ctx, messageId));
	JS_SetPropertyStr(_ctx, obj, "senderId", JS_NewInt64(_ctx, senderId));
	JS_SetPropertyStr(_ctx, obj, "senderName", JS_NewString(_ctx, senderName.constData()));
	JS_SetPropertyStr(_ctx, obj, "text", JS_NewString(_ctx, text.constData()));
	JS_SetPropertyStr(_ctx, obj, "out", JS_NewBool(_ctx, out ? 1 : 0));

	for (auto &fn : _messageHandlers) {
		JSValueConst args[1] = { obj };
		auto r = JS_Call(_ctx, fn, JS_UNDEFINED, 1, args);
		if (JS_IsException(r)) {
			const auto err = takeException();
			LOG(("[furry-script] handler error: %1").arg(err));
		}
		JS_FreeValue(_ctx, r);
	}
	JS_FreeValue(_ctx, obj);
}

// Parse a script-provided string into formatted text. Resolves [label](url) links
// ourselves (tdesktop markdown parser handles **bold**/`code` but NOT link syntax):
// tg://user?id=N -> clickable profile mention, otherwise -> text URL. Then runs the
// markdown parser for bold/italic/code/spoilers.
[[nodiscard]] TextWithEntities ParseScriptText(
		not_null<Main::Session*> session,
		const QString &text) {
	static const auto linkRegex = QRegularExpression(
		u"\\[([^\\]]+)\\]\\(([^)]+)\\)"_q);
	auto out = TextWithEntities();
	auto pos = 0;
	auto it = linkRegex.globalMatch(text);
	while (it.hasNext()) {
		const auto m = it.next();
		out.text += text.mid(pos, m.capturedStart() - pos);
		const auto label = m.captured(1);
		const auto url = m.captured(2);
		const auto offset = int(out.text.size());
		out.text += label;
		if (url.startsWith(u"tg://user?id="_q)) {
			const auto userId = url.mid(13).toULongLong();
			const auto data = TextUtilities::MentionNameDataFromFields({
				.selfId = session->userPeerId().value,
				.userId = userId,
			});
			out.entities.push_back({
				EntityType::MentionName, offset, int(label.size()), data });
		} else {
			out.entities.push_back({
				EntityType::CustomUrl, offset, int(label.size()), url });
		}
		pos = m.capturedEnd();
	}
	out.text += text.mid(pos);
	TextUtilities::ParseEntities(out, TextParseMarkdown);
	return out;
}

void Manager::doSendMessage(std::int64_t chatId, const QString &text) {
	if (!_session) {
		return;
	}
	const auto peerId = PeerId(BareId(chatId));
	const auto history = _session->data().history(peerId);
	auto message = Api::MessageToSend(Api::SendAction(history));
	const auto parsed = ParseScriptText(_session, text);
	message.textWithTags = TextWithTags{
		parsed.text,
		TextUtilities::ConvertEntitiesToTextTags(parsed.entities),
	};
	_session->api().sendMessage(std::move(message));
}

void Manager::doEditMessage(
		std::int64_t chatId,
		std::int64_t msgId,
		const QString &text) {
	if (!_session) {
		return;
	}
	const auto peerId = PeerId(BareId(chatId));
	const auto item = _session->data().message(peerId, MsgId(msgId));
	if (!item) {
		return;
	}
	const auto parsed = ParseScriptText(_session, text);
	Api::EditTextMessage(
		item,
		parsed,
		Data::WebPageDraft(),
		Api::SendOptions(),
		[](mtpRequestId) {},
		[](const QString &, mtpRequestId) {},
		false);
}

void Manager::doWriteFile(const QString &name, const QString &text) {
	// Confined to the scripts folder; strip any path separators from name.
	auto clean = name;
	clean.replace('/', '_').replace('\\', '_');
	if (clean.isEmpty()) {
		return;
	}
	QDir().mkpath(folder());
	auto file = QFile(folder() + u"/"_q + clean);
	if (file.open(QIODevice::Append | QIODevice::Text)) {
		file.write(text.toUtf8());
		file.write("\n");
		file.close();
	}
}

} // namespace

void Start() {
	if (GlobalManager) {
		return;
	}
	GlobalManager = new Manager();
	GlobalManager->loadScripts();
	LOG(("[furry-script] engine started"));
}

void Reload() {
	if (GlobalManager) {
		GlobalManager->reload();
		ChangedStream.fire({});
	}
}

std::vector<ScriptInfo> LoadedScripts() {
	return GlobalManager ? GlobalManager->scripts() : std::vector<ScriptInfo>();
}

void SetScriptEnabled(const QString &name, bool enabled) {
	if (GlobalManager) {
		GlobalManager->setEnabled(name, enabled);
	}
}

bool IsScriptEnabled(const QString &name) {
	// Read from the disabled-set (updated synchronously in setEnabled), not from
	// _scripts (rebuilt only on the deferred reload) — otherwise the label lags
	// one toggle behind and shows the inverted state.
	return GlobalManager ? GlobalManager->isEnabled(name) : false;
}

rpl::producer<> ScriptsChanged() {
	return ChangedStream.events();
}

QString ScriptsFolder() {
	const auto path = Manager::folder();
	QDir().mkpath(path);
	return path;
}

ScriptSettingsSchema ScriptSchema(const QString &name) {
	return GlobalManager ? GlobalManager->schema(name) : ScriptSettingsSchema();
}

bool GetScriptBool(const QString &name, const QString &key, bool fallback) {
	const auto obj = ReadValues(name);
	const auto v = obj.value(key);
	return v.isBool() ? v.toBool() : fallback;
}

double GetScriptNumber(const QString &name, const QString &key, double fallback) {
	const auto obj = ReadValues(name);
	const auto v = obj.value(key);
	return v.isDouble() ? v.toDouble() : fallback;
}

QString GetScriptText(const QString &name, const QString &key, const QString &fallback) {
	const auto obj = ReadValues(name);
	const auto v = obj.value(key);
	return v.isString() ? v.toString() : fallback;
}

void SetScriptBool(const QString &name, const QString &key, bool value) {
	auto obj = ReadValues(name);
	obj[key] = value;
	WriteValues(name, obj);
}

void SetScriptNumber(const QString &name, const QString &key, double value) {
	auto obj = ReadValues(name);
	obj[key] = value;
	WriteValues(name, obj);
}

void SetScriptText(const QString &name, const QString &key, const QString &value) {
	auto obj = ReadValues(name);
	obj[key] = value;
	WriteValues(name, obj);
}

} // namespace FurryScripts
