// FurryGram: local voice transcription via whisper.cpp (CPU, offline).
#include "ayu/features/voice_transcribe.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/furry_lang.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "base/call_delayed.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "settings.h" // cWorkingDir
#include "storage/cache/storage_cache_database.h"

#include <whisper.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
} // extern "C"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <rpl/variable.h>

#include <mutex>
#include <thread>

namespace Ayu::Voice {
namespace {

// Whisper expects 16 kHz mono float32 PCM.
constexpr auto kSampleRate = 16000;

// Local entries never go through MTP, so we never look them up by request id;
// a single shared sentinel is enough to mark the "loading" UI state.
[[nodiscard]] std::mutex &WhisperMutex() {
	static std::mutex mutex;
	return mutex;
}

struct BufferReader {
	const char *data = nullptr;
	int size = 0;
	int pos = 0;
};

int ReadPacket(void *opaque, uint8_t *buf, int bufSize) {
	const auto reader = static_cast<BufferReader*>(opaque);
	const auto remain = reader->size - reader->pos;
	if (remain <= 0) {
		return AVERROR_EOF;
	}
	const auto take = std::min(bufSize, remain);
	memcpy(buf, reader->data + reader->pos, take);
	reader->pos += take;
	return take;
}

int64_t SeekPacket(void *opaque, int64_t offset, int whence) {
	const auto reader = static_cast<BufferReader*>(opaque);
	if (whence == AVSEEK_SIZE) {
		return reader->size;
	}
	auto target = int64_t(-1);
	if (whence == SEEK_SET) {
		target = offset;
	} else if (whence == SEEK_CUR) {
		target = reader->pos + offset;
	} else if (whence == SEEK_END) {
		target = reader->size + offset;
	}
	if (target < 0 || target > reader->size) {
		return -1;
	}
	reader->pos = int(target);
	return target;
}

// Decode arbitrary compressed audio bytes (OGG/Opus for voice messages) into
// 16 kHz mono float32 samples using the bundled FFmpeg.
std::vector<float> DecodePcm(const QByteArray &bytes, bool &ok) {
	ok = false;
	auto out = std::vector<float>();

	auto reader = BufferReader{ bytes.constData(), int(bytes.size()), 0 };

	const auto ioBufferSize = 32768;
	auto ioBuffer = static_cast<unsigned char*>(av_malloc(ioBufferSize));
	if (!ioBuffer) {
		return out;
	}
	auto avio = avio_alloc_context(
		ioBuffer,
		ioBufferSize,
		0,
		&reader,
		&ReadPacket,
		nullptr,
		&SeekPacket);
	if (!avio) {
		av_free(ioBuffer);
		return out;
	}

	auto format = avformat_alloc_context();
	if (!format) {
		av_freep(&avio->buffer);
		avio_context_free(&avio);
		return out;
	}
	format->pb = avio;

	if (avformat_open_input(&format, nullptr, nullptr, nullptr) < 0) {
		// On failure avformat_open_input frees `format` itself.
		av_freep(&avio->buffer);
		avio_context_free(&avio);
		return out;
	}

	const auto cleanup = [&] {
		avformat_close_input(&format);
		if (avio) {
			av_freep(&avio->buffer);
			avio_context_free(&avio);
		}
	};

	if (avformat_find_stream_info(format, nullptr) < 0) {
		cleanup();
		return out;
	}

	auto streamIndex = -1;
	for (auto i = 0u; i < format->nb_streams; ++i) {
		if (format->streams[i]->codecpar->codec_type
			== AVMEDIA_TYPE_AUDIO) {
			streamIndex = int(i);
			break;
		}
	}
	if (streamIndex < 0) {
		cleanup();
		return out;
	}

	const auto stream = format->streams[streamIndex];
	const auto decoder = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!decoder) {
		cleanup();
		return out;
	}
	auto codec = avcodec_alloc_context3(decoder);
	if (!codec) {
		cleanup();
		return out;
	}
	if (avcodec_parameters_to_context(codec, stream->codecpar) < 0
		|| avcodec_open2(codec, decoder, nullptr) < 0) {
		avcodec_free_context(&codec);
		cleanup();
		return out;
	}

	AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_MONO;
	auto swr = static_cast<SwrContext*>(nullptr);
	if (swr_alloc_set_opts2(
			&swr,
			&outLayout,
			AV_SAMPLE_FMT_FLT,
			kSampleRate,
			&codec->ch_layout,
			codec->sample_fmt,
			codec->sample_rate,
			0,
			nullptr) < 0
		|| swr_init(swr) < 0) {
		if (swr) {
			swr_free(&swr);
		}
		avcodec_free_context(&codec);
		cleanup();
		return out;
	}

	auto packet = av_packet_alloc();
	auto frame = av_frame_alloc();

	const auto drainFrame = [&] {
		const auto maxOut = swr_get_out_samples(swr, frame->nb_samples);
		if (maxOut <= 0) {
			return;
		}
		auto buffer = static_cast<uint8_t*>(nullptr);
		if (av_samples_alloc(
				&buffer,
				nullptr,
				1,
				maxOut,
				AV_SAMPLE_FMT_FLT,
				0) < 0) {
			return;
		}
		const auto converted = swr_convert(
			swr,
			&buffer,
			maxOut,
			const_cast<const uint8_t**>(frame->extended_data),
			frame->nb_samples);
		if (converted > 0) {
			const auto samples = reinterpret_cast<const float*>(buffer);
			out.insert(out.end(), samples, samples + converted);
		}
		av_freep(&buffer);
	};

	while (av_read_frame(format, packet) >= 0) {
		if (packet->stream_index == streamIndex) {
			if (avcodec_send_packet(codec, packet) >= 0) {
				while (avcodec_receive_frame(codec, frame) >= 0) {
					drainFrame();
				}
			}
		}
		av_packet_unref(packet);
	}

	// Flush the decoder.
	avcodec_send_packet(codec, nullptr);
	while (avcodec_receive_frame(codec, frame) >= 0) {
		drainFrame();
	}

	av_frame_free(&frame);
	av_packet_free(&packet);
	swr_free(&swr);
	avcodec_free_context(&codec);
	cleanup();

	ok = !out.empty();
	return out;
}

// Options carried from the main thread into the whisper worker.
struct RunOptions {
	QString modelPath;
	QString language; // "auto" or a 2-letter code.
	bool translate = false;
};

// Cached whisper model (guarded by WhisperMutex()).
whisper_context *GContext = nullptr;
QString GLoadedPath;

// Frees the cached model. Must be called under WhisperMutex().
void FreeContextLocked() {
	if (GContext) {
		whisper_free(GContext);
		GContext = nullptr;
		GLoadedPath.clear();
	}
}

void SilentLog(ggml_log_level, const char *, void *) {
}

// Silence whisper/ggml stdout+stderr spam (once, under WhisperMutex()).
void SilenceLogsOnce() {
	static auto done = false;
	if (!done) {
		done = true;
		whisper_log_set(&SilentLog, nullptr);
		ggml_log_set(&SilentLog, nullptr);
	}
}

// Loads (and caches) the whisper model. Must be called under WhisperMutex().
whisper_context *AcquireContext(const QString &modelPath) {
	SilenceLogsOnce();
	if (GContext && GLoadedPath == modelPath) {
		return GContext;
	}
	FreeContextLocked();
	auto params = whisper_context_default_params();
	params.use_gpu = false;
	const auto utf8 = modelPath.toUtf8();
	GContext = whisper_init_from_file_with_params(utf8.constData(), params);
	if (GContext) {
		GLoadedPath = modelPath;
	}
	return GContext;
}

TranscribeResult RunWhisper(const QByteArray &audio, const RunOptions &options) {
	auto result = TranscribeResult();

	auto decoded = false;
	auto pcm = DecodePcm(audio, decoded);
	if (!decoded || pcm.empty()) {
		result.failed = true;
		return result;
	}

	auto lock = std::lock_guard(WhisperMutex());
	const auto context = AcquireContext(options.modelPath);
	if (!context) {
		result.failed = true;
		return result;
	}

	auto params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
	params.print_progress = false;
	params.print_realtime = false;
	params.print_timestamps = false;
	params.print_special = false;
	params.translate = options.translate;
	params.no_timestamps = true;
	// `language` must stay alive for the whole whisper_full call.
	const auto languageUtf8 = options.language.isEmpty()
		? QByteArray("auto")
		: options.language.toUtf8();
	params.language = languageUtf8.constData();
	const auto cores = std::thread::hardware_concurrency();
	params.n_threads = std::max(1, int(cores ? (cores / 2) : 4));

	if (whisper_full(context, params, pcm.data(), int(pcm.size())) != 0) {
		result.failed = true;
		return result;
	}

	auto text = QString();
	const auto segments = whisper_full_n_segments(context);
	for (auto i = 0; i < segments; ++i) {
		text += QString::fromUtf8(whisper_full_get_segment_text(context, i));
	}
	result.text = text.trimmed();
	if (result.text.isEmpty()) {
		result.text = FurryLang::Pick(
			u"(no speech)"_q,
			QString::fromUtf8("(\xD1\x82\xD0\xB8\xD1\x88\xD0\xB8\xD0\xBD\xD0\xB0)")); // (тишина)
	}
	return result;
}

// Decode + run whisper on a worker thread, deliver the result on the main one.
void RunAsync(
		QByteArray bytes,
		RunOptions options,
		Fn<void(TranscribeResult)> done) {
	crl::async([done = std::move(done), options = std::move(options),
			bytes = std::move(bytes)]() mutable {
		auto result = RunWhisper(bytes, options);
		crl::on_main([done = std::move(done),
				result = std::move(result)]() mutable {
			done(result);
		});
	});
}

constexpr auto kFetchAttempts = 8;
constexpr auto kFetchRetryDelay = crl::time(400);

// "Voice message is loading…" / "Голосовое загружается…"
[[nodiscard]] QString LoadingMessage() {
	return FurryLang::Pick(
		u"Voice message is loading..."_q,
		QString::fromUtf8(
			"\xD0\x93\xD0\xBE\xD0\xBB\xD0\xBE\xD1\x81\xD0\xBE\xD0\xB2\xD0\xBE\xD0\xB5"
			" "
			"\xD0\xB7\xD0\xB0\xD0\xB3\xD1\x80\xD1\x83\xD0\xB6\xD0\xB0\xD0\xB5\xD1\x82"
			"\xD1\x81\xD1\x8F\xE2\x80\xA6"));
}

// Obtain the voice audio bytes (memory / disk / cache), forcing a download and
// retrying if needed, then run whisper. Always invoked on the main thread.
void TryFetchAndRun(
		not_null<Data::Session*> owner,
		FullMsgId id,
		RunOptions options,
		Fn<void(TranscribeResult)> done,
		int attemptsLeft) {
	const auto item = owner->message(id);
	const auto media = item ? item->media() : nullptr;
	const auto document = media ? media->document() : nullptr;
	if (!document) {
		done({ .failed = true });
		return;
	}

	// 1) Bytes already in memory (loaded via loader this session).
	const auto view = document->createMediaView();
	auto bytes = view->bytes();
	if (!bytes.isEmpty()) {
		RunAsync(std::move(bytes), std::move(options), std::move(done));
		return;
	}

	// 2) Saved to an actual file on disk.
	const auto path = document->filepath(true);
	if (!path.isEmpty()) {
		auto file = QFile(path);
		if (file.open(QIODevice::ReadOnly)) {
			bytes = file.readAll();
		}
		if (!bytes.isEmpty()) {
			RunAsync(std::move(bytes), std::move(options), std::move(done));
			return;
		}
	}

	// 3) Read from the local cache (voice content lives there once fully
	// loaded). The cache callback runs on a worker thread — hop to main.
	owner->cache().get(document->cacheKey(), [=](QByteArray &&value) mutable {
		crl::on_main([=, value = std::move(value)]() mutable {
			if (!value.isEmpty() && !value.startsWith("partial:")) {
				RunAsync(std::move(value), options, done);
				return;
			}
			if (attemptsLeft > 0) {
				// Force a full download to the cache, then retry shortly.
				if (const auto item = owner->message(id)) {
					if (const auto media = item->media()) {
						if (const auto document = media->document()) {
							document->save(
								Data::FileOrigin(id),
								QString());
						}
					}
				}
				base::call_delayed(
					kFetchRetryDelay,
					base::make_weak(&owner->session()),
					[=] {
						TryFetchAndRun(
							owner,
							id,
							options,
							done,
							attemptsLeft - 1);
					});
				return;
			}
			auto result = TranscribeResult();
			result.text = LoadingMessage();
			done(result);
		});
	});
}

// Transcriptions waiting for the model to finish downloading (main thread only).
struct PendingRun {
	not_null<Data::Session*> owner;
	FullMsgId id;
	RunOptions options;
	Fn<void(TranscribeResult)> done;
};

[[nodiscard]] std::vector<PendingRun> &PendingRuns() {
	static auto runs = std::vector<PendingRun>();
	return runs;
}

// Called when a model download settles: run (or fail) everything that waited.
void FlushPendingRuns(bool ready) {
	auto runs = std::vector<PendingRun>();
	runs.swap(PendingRuns());
	for (auto &run : runs) {
		if (ready) {
			TryFetchAndRun(
				run.owner,
				run.id,
				std::move(run.options),
				std::move(run.done),
				kFetchAttempts);
		} else {
			run.done({ .failed = true });
		}
	}
}

} // namespace

bool LocalTranscribeEnabled() {
	return AyuSettings::getInstance().localTranscribe();
}

QString ModelSize() {
	const auto size = AyuSettings::getInstance().whisperModel();
	return size.isEmpty() ? u"base"_q : size;
}

QString ModelsDir() {
	return cWorkingDir() + u"tdata/furry_whisper/"_q;
}

QString ModelPath() {
	return ModelsDir() + u"ggml-"_q + ModelSize() + u".bin"_q;
}

bool ModelAvailable() {
	return QFile::exists(ModelPath());
}

void FreeContext() {
	auto lock = std::lock_guard(WhisperMutex());
	FreeContextLocked();
}

void Transcribe(
		not_null<HistoryItem*> item,
		Fn<void(TranscribeResult)> done) {
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document) {
		done({ .failed = true });
		return;
	}
	auto &settings = AyuSettings::getInstance();
	auto options = RunOptions{
		.modelPath = ModelPath(),
		.language = settings.whisperLanguage(),
		.translate = settings.whisperTranslate(),
	};

	if (!ModelAvailable()) {
		// Auto-start the download on first use. Only queue this transcription
		// if a download is actually in flight, otherwise report failure.
		StartModelDownload();
		if (CurrentModelStatus() != ModelStatus::Downloading) {
			done({ .failed = true });
			return;
		}
		PendingRuns().push_back({
			&document->owner(),
			item->fullId(),
			std::move(options),
			done,
		});
		auto result = TranscribeResult();
		result.text = FurryLang::Pick(
			u"Downloading the Whisper model..."_q,
			QString::fromUtf8(
				"\xD0\xA1\xD0\xBA\xD0\xB0\xD1\x87\xD0\xB8\xD0\xB2\xD0\xB0\xD1\x8E"
				" \xD0\xBC\xD0\xBE\xD0\xB4\xD0\xB5\xD0\xBB\xD1\x8C Whisper\xE2\x80"
				"\xA6"));
		done(result);
		return;
	}

	TryFetchAndRun(
		&document->owner(),
		item->fullId(),
		std::move(options),
		std::move(done),
		kFetchAttempts);
}

namespace {

// Singleton controller that downloads the selected ggml model (CPU build) from
// HuggingFace into ModelsDir(), exposing reactive progress for the settings UI.
class ModelController final {
public:
	[[nodiscard]] static ModelController &Instance() {
		static auto instance = ModelController();
		return instance;
	}

	[[nodiscard]] rpl::producer<ModelProgress> value() const {
		return _status.value();
	}
	[[nodiscard]] ModelStatus status() const {
		return _status.current().status;
	}

	void refresh() {
		if (_reply) {
			return; // Mid-download — keep the downloading state.
		}
		_status = ModelProgress{
			ModelAvailable() ? ModelStatus::Ready : ModelStatus::Missing,
			0 };
	}

	void download() {
		if (_reply) {
			return;
		}
		const auto dir = ModelsDir();
		QDir().mkpath(dir);
		_finalPath = ModelPath();
		_partPath = _finalPath + u".part"_q;

		// Resume from a previously interrupted .part if one is present.
		_resumeOffset = QFileInfo::exists(_partPath)
			? QFileInfo(_partPath).size()
			: 0;
		_handledStatus = false;
		_file = std::make_unique<QFile>(_partPath);
		const auto mode = (_resumeOffset > 0)
			? (QIODevice::WriteOnly | QIODevice::Append)
			: (QIODevice::WriteOnly | QIODevice::Truncate);
		if (!_file->open(mode)) {
			_file = nullptr;
			fail();
			return;
		}
		const auto url = u"https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-"_q
			+ ModelSize()
			+ u".bin"_q;
		auto request = QNetworkRequest(QUrl(url));
		request.setAttribute(
			QNetworkRequest::RedirectPolicyAttribute,
			QNetworkRequest::NoLessSafeRedirectPolicy);
		if (_resumeOffset > 0) {
			request.setRawHeader(
				QByteArrayLiteral("Range"),
				"bytes=" + QByteArray::number(_resumeOffset) + "-");
		}
		_status = ModelProgress{ ModelStatus::Downloading, 0 };
		_reply = _manager.get(request);
		QObject::connect(_reply, &QNetworkReply::readyRead, _reply, [this] {
			if (_file && _reply) {
				handleStatusOnce();
				_file->write(_reply->readAll());
			}
		});
		QObject::connect(
			_reply,
			&QNetworkReply::downloadProgress,
			_reply,
			[this](qint64 received, qint64 total) {
				const auto whole = (total > 0) ? (_resumeOffset + total) : 0;
				const auto got = _resumeOffset + received;
				const auto pct = (whole > 0) ? int((got * 100) / whole) : 0;
				_status = ModelProgress{ ModelStatus::Downloading, pct };
			});
		QObject::connect(_reply, &QNetworkReply::finished, _reply, [this] {
			if (!_reply) {
				return;
			}
			const auto networkError =
				(_reply->error() != QNetworkReply::NoError);
			if (_file) {
				handleStatusOnce();
				_file->write(_reply->readAll());
				_file->close();
			}
			_reply->deleteLater();
			_reply = nullptr;
			if (networkError) {
				// Keep the .part so the next attempt resumes from here.
				_file = nullptr;
				_status = ModelProgress{ ModelStatus::Missing, 0 };
				FlushPendingRuns(false);
				return;
			}
			_file = nullptr;
			// Integrity: a complete model is at least ~85% of its expected
			// size. Catches truncation or an error page sent with HTTP 200.
			const auto have = QFileInfo(_partPath).size();
			const auto minBytes =
				qint64(ModelSizeMb(ModelSize())) * 1024 * 1024 * 85 / 100;
			if (have < minBytes) {
				QFile::remove(_partPath); // Corrupt — restart next time.
				_status = ModelProgress{ ModelStatus::Missing, 0 };
				FlushPendingRuns(false);
				return;
			}
			QFile::remove(_finalPath);
			if (QFile::rename(_partPath, _finalPath)) {
				_status = ModelProgress{ ModelStatus::Ready, 100 };
				FlushPendingRuns(true);
			} else {
				fail();
			}
		});
	}

	// On the first chunk, if we asked for a byte range but the server replied
	// with the full file (not 206), restart writing from offset 0.
	void handleStatusOnce() {
		if (_handledStatus || !_reply || !_file) {
			return;
		}
		_handledStatus = true;
		if (_resumeOffset > 0) {
			const auto status = _reply->attribute(
				QNetworkRequest::HttpStatusCodeAttribute).toInt();
			if (status != 206) {
				_file->seek(0);
				_file->resize(0);
				_resumeOffset = 0;
			}
		}
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
		if (!_partPath.isEmpty()) {
			QFile::remove(_partPath);
		}
		refresh();
		FlushPendingRuns(false);
	}

	void remove() {
		cancel();
		QFile::remove(ModelPath());
		refresh();
	}

private:
	void fail() {
		if (_file) {
			_file->close();
			_file = nullptr;
		}
		if (!_partPath.isEmpty()) {
			QFile::remove(_partPath);
		}
		_status = ModelProgress{ ModelStatus::Missing, 0 };
		FlushPendingRuns(false);
	}

	rpl::variable<ModelProgress> _status = ModelProgress{
		ModelAvailable() ? ModelStatus::Ready : ModelStatus::Missing,
		0 };
	QNetworkAccessManager _manager;
	QNetworkReply *_reply = nullptr;
	std::unique_ptr<QFile> _file;
	QString _partPath;
	QString _finalPath;
	qint64 _resumeOffset = 0;
	bool _handledStatus = false;
};

} // namespace

int ModelSizeMb(const QString &size) {
	if (size == u"small"_q) {
		return 488;
	} else if (size == u"medium"_q) {
		return 1530;
	}
	return 148; // base
}

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
	FreeContext();
	ModelController::Instance().remove();
}

} // namespace Ayu::Voice
