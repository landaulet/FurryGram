// FurryGram: downloader for the local CLIP model (single ~290 MB gguf).
// Mirrors the whisper ModelController: reactive status for the settings UI,
// resumable download (HTTP Range), integrity check, .part -> rename.
#include "ayu/features/media_clip.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <rpl/variable.h>

#include <memory>

namespace Ayu::Clip {
namespace {

// Two-tower CLIP in monatis/ggml format (f16) for the currently selected model.
[[nodiscard]] QString ModelUrl() {
	return (CurrentModel() == u"l14"_q)
		// ViT-L/14 (LAION-2B) — stronger retrieval, ~860 MB.
		? u"https://huggingface.co/mys/ggml_CLIP-ViT-L-14-laion2B-s32B-b82K/"
			u"resolve/main/CLIP-ViT-L-14-laion2B-s32B-b82K_ggml-model-f16.gguf"_q
		// ViT-B/32 — fast, ~290 MB.
		: u"https://huggingface.co/mys/ggml_clip-vit-base-patch32/resolve/main/"
			u"clip-vit-base-patch32_ggml-model-f16.gguf"_q;
}

// A valid model is hundreds of MB; far smaller means a truncated/error response.
[[nodiscard]] qint64 MinValidBytes() {
	return (CurrentModel() == u"l14"_q)
		? qint64(600) * 1024 * 1024
		: qint64(200) * 1024 * 1024;
}

// Singleton controller for the one CLIP model file.
class ModelController final {
public:
	[[nodiscard]] static ModelController &Instance() {
		static auto instance = ModelController();
		return instance;
	}

	[[nodiscard]] rpl::producer<ModelProgress> value() {
		return _status.value();
	}
	[[nodiscard]] ModelStatus status() {
		return _status.current().status;
	}

	void refresh() {
		if (_reply) {
			return;
		}
		_status = ModelProgress{
			ModelAvailable() ? ModelStatus::Ready : ModelStatus::Missing,
			0 };
	}

	void download() {
		if (_reply) {
			return;
		}
		QDir().mkpath(ModelsDir());
		_finalPath = ModelPath();
		_partPath = _finalPath + u".part"_q;

		// Resume from an existing .part if present.
		const auto have = QFileInfo(_partPath).size();
		_file = std::make_unique<QFile>(_partPath);
		const auto mode = (have > 0)
			? (QIODevice::WriteOnly | QIODevice::Append)
			: (QIODevice::WriteOnly | QIODevice::Truncate);
		if (!_file->open(mode)) {
			_file = nullptr;
			fail();
			return;
		}

		auto request = QNetworkRequest(QUrl(ModelUrl()));
		request.setAttribute(
			QNetworkRequest::RedirectPolicyAttribute,
			QNetworkRequest::NoLessSafeRedirectPolicy);
		if (have > 0) {
			const auto range = QString("bytes=%1-").arg(have);
			request.setRawHeader("Range", range.toUtf8());
		}
		_status = ModelProgress{ ModelStatus::Downloading, 0 };
		_baseBytes = have;
		_reply = _manager.get(request);
		const auto reply = _reply;

		QObject::connect(reply, &QNetworkReply::readyRead, reply, [=] {
			if (_file && _reply == reply) {
				_file->write(reply->readAll());
			}
		});
		QObject::connect(
			reply,
			&QNetworkReply::downloadProgress,
			reply,
			[=](qint64 received, qint64 total) {
				if (_reply != reply) {
					return;
				}
				const auto grand = _baseBytes + total;
				const auto done = _baseBytes + received;
				const auto pct = (grand > 0)
					? int((done * 100) / grand)
					: 0;
				_status = ModelProgress{ ModelStatus::Downloading, pct };
			});
		QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
			if (_reply != reply) {
				return;
			}
			const auto networkError =
				(reply->error() != QNetworkReply::NoError);
			if (_file) {
				_file->write(reply->readAll());
				_file->close();
			}
			reply->deleteLater();
			_reply = nullptr;
			_file = nullptr;
			if (networkError) {
				// Keep .part so a retry resumes via Range.
				_status = ModelProgress{ ModelStatus::Missing, 0 };
				return;
			}
			if (QFileInfo(_partPath).size() < MinValidBytes()) {
				QFile::remove(_partPath);
				_status = ModelProgress{ ModelStatus::Missing, 0 };
				return;
			}
			QFile::remove(_finalPath);
			if (QFile::rename(_partPath, _finalPath)) {
				_status = ModelProgress{ ModelStatus::Ready, 100 };
			} else {
				fail();
			}
		});
	}

	void cancel() {
		if (_reply) {
			const auto reply = _reply;
			_reply = nullptr;
			reply->disconnect();
			reply->abort();
			reply->deleteLater();
		}
		if (_file) {
			_file->close();
			_file = nullptr;
		}
		// Keep the .part so the next download resumes.
		refresh();
	}

	void remove() {
		cancel();
		if (!_partPath.isEmpty()) {
			QFile::remove(_partPath);
		}
		QFile::remove(ModelPath());
		FreeContext();
		refresh();
	}

private:
	ModelController() {
		_status = ModelProgress{
			ModelAvailable() ? ModelStatus::Ready : ModelStatus::Missing,
			0 };
	}

	void fail() {
		if (_file) {
			_file->close();
			_file = nullptr;
		}
		if (!_partPath.isEmpty()) {
			QFile::remove(_partPath);
		}
		_status = ModelProgress{ ModelStatus::Missing, 0 };
	}

	rpl::variable<ModelProgress> _status;
	QNetworkAccessManager _manager;
	QNetworkReply *_reply = nullptr;
	std::unique_ptr<QFile> _file;
	QString _partPath;
	QString _finalPath;
	qint64 _baseBytes = 0;
};

} // namespace

rpl::producer<ModelProgress> ModelStatusValue() {
	return ModelController::Instance().value();
}

ModelStatus CurrentModelStatus() {
	return ModelController::Instance().status();
}

void RefreshModelStatus() {
	ModelController::Instance().refresh();
}

void StartModelDownload() {
	ModelController::Instance().download();
}

void CancelModelDownload() {
	ModelController::Instance().cancel();
}

void DeleteModel() {
	ModelController::Instance().remove();
}

} // namespace Ayu::Clip
