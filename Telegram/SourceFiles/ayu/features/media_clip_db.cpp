// FurryGram: persistent cache of CLIP image embeddings (per-chat search).
#include "ayu/features/media_clip_db.h"

#include "ayu/libs/sqlite/sqlite_orm.h"

#include <cstring>
#include <mutex>

namespace Ayu::ClipDb {
namespace {

using namespace sqlite_orm;

// One cached embedding. emb/thumb are BLOBs; emb is raw float32 (full
// precision — int8 quantization measurably hurt discrimination on CLIP's
// small-magnitude unit vectors).
struct Row {
	int64 fakeId = 0;
	int64 peerId = 0;
	int kind = 0;
	int64 mediaId = 0;
	int64 msgId = 0;
	int dim = 0;
	std::vector<char> emb;
	std::vector<char> thumb;
};

[[nodiscard]] std::mutex &Mutex() {
	static auto mutex = std::mutex();
	return mutex;
}

// Function-local storage so the DB file opens lazily on first use.
[[nodiscard]] auto &Storage() {
	// v3 db: added msgId column (so search can list & jump to cached photos that
	// aren't currently resident). Fresh file to avoid migrating old rows.
	static auto storage = make_storage(
		"./tdata/furry_clip3.db",
		make_index("idx_clip_peerId", &Row::peerId),
		make_table<Row>(
			"MediaEmbedding",
			make_column("fakeId", &Row::fakeId, primary_key().autoincrement()),
			make_column("peerId", &Row::peerId),
			make_column("kind", &Row::kind),
			make_column("mediaId", &Row::mediaId),
			make_column("msgId", &Row::msgId),
			make_column("dim", &Row::dim),
			make_column("emb", &Row::emb),
			make_column("thumb", &Row::thumb)));
	return storage;
}

bool gInited = false;

// Must be called under Mutex().
void EnsureInit() {
	if (gInited) {
		return;
	}
	gInited = true;
	try {
		auto &s = Storage();
		s.sync_schema(true);
		// WAL: readers never block the (rare) writer; small busy timeout for the
		// off-chance two searches race.
		s.pragma.journal_mode(journal_mode::WAL);
		s.busy_timeout(3000);
	} catch (...) {
	}
}

[[nodiscard]] std::vector<char> FloatsToBytes(const std::vector<float> &v) {
	auto out = std::vector<char>(v.size() * sizeof(float));
	if (!v.empty()) {
		std::memcpy(out.data(), v.data(), out.size());
	}
	return out;
}

[[nodiscard]] std::vector<float> BytesToFloats(const std::vector<char> &b) {
	auto out = std::vector<float>(b.size() / sizeof(float));
	if (!out.empty()) {
		std::memcpy(out.data(), b.data(), out.size() * sizeof(float));
	}
	return out;
}

} // namespace

std::vector<Entry> LoadPeer(int64 peerId) {
	auto lock = std::lock_guard(Mutex());
	auto out = std::vector<Entry>();
	try {
		EnsureInit();
		auto rows = Storage().get_all<Row>(
			where(c(&Row::peerId) == peerId));
		out.reserve(rows.size());
		for (auto &r : rows) {
			auto e = Entry();
			e.kind = r.kind;
			e.mediaId = r.mediaId;
			e.msgId = r.msgId;
			e.emb = BytesToFloats(r.emb);
			e.thumb = QByteArray(r.thumb.data(), int(r.thumb.size()));
			out.push_back(std::move(e));
		}
	} catch (...) {
	}
	return out;
}

void StoreMany(int64 peerId, const std::vector<Entry> &entries) {
	if (entries.empty()) {
		return;
	}
	auto lock = std::lock_guard(Mutex());
	try {
		EnsureInit();
		auto &s = Storage();
		s.begin_transaction();
		for (const auto &e : entries) {
			auto r = Row();
			r.peerId = peerId;
			r.kind = e.kind;
			r.mediaId = e.mediaId;
			r.msgId = e.msgId;
			r.dim = int(e.emb.size());
			r.emb = FloatsToBytes(e.emb);
			r.thumb = std::vector<char>(e.thumb.begin(), e.thumb.end());
			s.insert(r);
		}
		s.commit();
	} catch (...) {
		try {
			Storage().rollback();
		} catch (...) {
		}
	}
}

void ClearPeer(int64 peerId) {
	auto lock = std::lock_guard(Mutex());
	try {
		EnsureInit();
		Storage().remove_all<Row>(where(c(&Row::peerId) == peerId));
	} catch (...) {
	}
}

} // namespace Ayu::ClipDb
