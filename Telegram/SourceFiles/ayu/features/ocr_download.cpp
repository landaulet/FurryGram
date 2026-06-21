// FurryGram: per-language traineddata downloader for the local OCR engine.
// Mirrors the whisper ModelController, but keyed per language (eng/rus/...).
#include "ayu/features/ocr.h"

#include "settings.h" // cWorkingDir

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <rpl/variable.h>

#include <map>
#include <memory>

namespace Ayu::Ocr {
namespace {

[[nodiscard]] QString LangPath(const QString &lang) {
	return TessdataDir() + lang + u".traineddata"_q;
}

// Singleton that downloads tessdata_best <lang>.traineddata into TessdataDir(),
// exposing reactive per-language progress for the settings UI.
class LangController final {
public:
	[[nodiscard]] static LangController &Instance() {
		static auto instance = LangController();
		return instance;
	}

	[[nodiscard]] rpl::producer<LangProgress> value(const QString &lang) {
		return entry(lang).status.value();
	}
	[[nodiscard]] LangStatus status(const QString &lang) {
		return entry(lang).status.current().status;
	}

	void refresh(const QString &lang) {
		auto &e = entry(lang);
		if (e.reply) {
			return;
		}
		e.status = LangProgress{
			QFile::exists(LangPath(lang)) ? LangStatus::Ready : LangStatus::Missing,
			0 };
	}

	void download(const QString &lang) {
		auto &e = entry(lang);
		if (e.reply) {
			return;
		}
		QDir().mkpath(TessdataDir());
		e.finalPath = LangPath(lang);
		e.partPath = e.finalPath + u".part"_q;
		e.file = std::make_unique<QFile>(e.partPath);
		if (!e.file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			e.file = nullptr;
			fail(lang);
			return;
		}
		const auto url = u"https://github.com/tesseract-ocr/tessdata_best/raw/main/"_q
			+ lang
			+ u".traineddata"_q;
		auto request = QNetworkRequest(QUrl(url));
		request.setAttribute(
			QNetworkRequest::RedirectPolicyAttribute,
			QNetworkRequest::NoLessSafeRedirectPolicy);
		e.status = LangProgress{ LangStatus::Downloading, 0 };
		e.reply = _manager.get(request);
		const auto reply = e.reply;
		QObject::connect(reply, &QNetworkReply::readyRead, reply,
				[this, lang, reply] {
			auto &e = entry(lang);
			if (e.file && e.reply == reply) {
				e.file->write(reply->readAll());
			}
		});
		QObject::connect(
			reply,
			&QNetworkReply::downloadProgress,
			reply,
			[this, lang, reply](qint64 received, qint64 total) {
				auto &e = entry(lang);
				if (e.reply != reply) {
					return;
				}
				const auto pct = (total > 0)
					? int((received * 100) / total)
					: 0;
				e.status = LangProgress{ LangStatus::Downloading, pct };
			});
		QObject::connect(reply, &QNetworkReply::finished, reply,
				[this, lang, reply] {
			auto &e = entry(lang);
			if (e.reply != reply) {
				return;
			}
			const auto networkError =
				(reply->error() != QNetworkReply::NoError);
			if (e.file) {
				e.file->write(reply->readAll());
				e.file->close();
			}
			reply->deleteLater();
			e.reply = nullptr;
			if (networkError) {
				// Keep .part so a retry resumes (Range support is upstream's).
				e.file = nullptr;
				e.status = LangProgress{ LangStatus::Missing, 0 };
				return;
			}
			e.file = nullptr;
			// Integrity: a real traineddata is well over 1 MB; a tiny file means
			// an error page slipped through with HTTP 200.
			if (QFileInfo(e.partPath).size() < 1024 * 1024) {
				QFile::remove(e.partPath);
				e.status = LangProgress{ LangStatus::Missing, 0 };
				return;
			}
			QFile::remove(e.finalPath);
			if (QFile::rename(e.partPath, e.finalPath)) {
				e.status = LangProgress{ LangStatus::Ready, 100 };
			} else {
				fail(lang);
			}
		});
	}

	void cancel(const QString &lang) {
		auto &e = entry(lang);
		if (e.reply) {
			const auto reply = e.reply;
			e.reply = nullptr;
			reply->disconnect();
			reply->abort();
			reply->deleteLater();
		}
		if (e.file) {
			e.file->close();
			e.file = nullptr;
		}
		if (!e.partPath.isEmpty()) {
			QFile::remove(e.partPath);
		}
		refresh(lang);
	}

	void remove(const QString &lang) {
		cancel(lang);
		QFile::remove(LangPath(lang));
		refresh(lang);
	}

private:
	struct Entry {
		rpl::variable<LangProgress> status;
		QNetworkReply *reply = nullptr;
		std::unique_ptr<QFile> file;
		QString partPath;
		QString finalPath;
	};

	// unique_ptr keeps each Entry's rpl::variable address stable for producers.
	[[nodiscard]] Entry &entry(const QString &lang) {
		auto i = _entries.find(lang);
		if (i == _entries.end()) {
			i = _entries.emplace(lang, std::make_unique<Entry>()).first;
			i->second->status = LangProgress{
				QFile::exists(LangPath(lang))
					? LangStatus::Ready
					: LangStatus::Missing,
				0 };
		}
		return *i->second;
	}

	void fail(const QString &lang) {
		auto &e = entry(lang);
		if (e.file) {
			e.file->close();
			e.file = nullptr;
		}
		if (!e.partPath.isEmpty()) {
			QFile::remove(e.partPath);
		}
		e.status = LangProgress{ LangStatus::Missing, 0 };
	}

	QNetworkAccessManager _manager;
	std::map<QString, std::unique_ptr<Entry>> _entries;
};

} // namespace

QString TessdataDir() {
	return cWorkingDir() + u"tdata/furry_ocr/tessdata/"_q;
}

rpl::producer<LangProgress> LangStatusValue(const QString &lang) {
	return LangController::Instance().value(lang);
}

LangStatus CurrentLangStatus(const QString &lang) {
	return LangController::Instance().status(lang);
}

void RefreshLangStatus(const QString &lang) {
	LangController::Instance().refresh(lang);
}

void DownloadLang(const QString &lang) {
	LangController::Instance().download(lang);
}

void CancelLangDownload(const QString &lang) {
	LangController::Instance().cancel(lang);
}

void DeleteLang(const QString &lang) {
	LangController::Instance().remove(lang);
}

} // namespace Ayu::Ocr
