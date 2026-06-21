// FurryGram: image steganography (hide an encrypted message inside an image).
#include "ayu/features/furry_stego.h"

#include <QtCore/QBuffer>
#include <QtCore/QCryptographicHash>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstring>

namespace FurryStego {
namespace {

constexpr char kMagic[4] = { 'F', 'G', 'S', 'T' };
constexpr quint8 kVersion = 1;
constexpr quint8 kFlagEncrypted = 0x01;
constexpr auto kSaltSize = 16;
constexpr auto kIvSize = 16;
constexpr auto kShaSize = 32;
constexpr auto kPbkdf2Iterations = 100'000;
constexpr auto kLengthPrefixBytes = 4; // uint32 BE frame length

[[nodiscard]] QByteArray U32BE(quint32 value) {
	auto out = QByteArray(4, char(0));
	out[0] = char((value >> 24) & 0xFF);
	out[1] = char((value >> 16) & 0xFF);
	out[2] = char((value >> 8) & 0xFF);
	out[3] = char(value & 0xFF);
	return out;
}

[[nodiscard]] quint32 ReadU32BE(const uchar *p) {
	return (quint32(p[0]) << 24)
		| (quint32(p[1]) << 16)
		| (quint32(p[2]) << 8)
		| quint32(p[3]);
}

[[nodiscard]] QByteArray DeriveKey(
		const QString &password,
		const QByteArray &salt) {
	const auto pwd = password.toUtf8();
	auto key = QByteArray(32, char(0));
	if (PKCS5_PBKDF2_HMAC(
			pwd.constData(),
			pwd.size(),
			reinterpret_cast<const uchar*>(salt.constData()),
			salt.size(),
			kPbkdf2Iterations,
			EVP_sha256(),
			key.size(),
			reinterpret_cast<uchar*>(key.data())) != 1) {
		return QByteArray();
	}
	return key;
}

[[nodiscard]] QByteArray AesCbc(
		bool encrypt,
		const QByteArray &input,
		const QByteArray &key,
		const QByteArray &iv) {
	if (key.size() != 32 || iv.size() != kIvSize) {
		return QByteArray();
	}
	const auto ctx = EVP_CIPHER_CTX_new();
	if (!ctx) {
		return QByteArray();
	}
	const auto guard = gsl::finally([&] { EVP_CIPHER_CTX_free(ctx); });

	const auto keyPtr = reinterpret_cast<const uchar*>(key.constData());
	const auto ivPtr = reinterpret_cast<const uchar*>(iv.constData());
	if (EVP_CipherInit_ex(
			ctx,
			EVP_aes_256_cbc(),
			nullptr,
			keyPtr,
			ivPtr,
			encrypt ? 1 : 0) != 1) {
		return QByteArray();
	}

	auto out = QByteArray(input.size() + EVP_MAX_BLOCK_LENGTH, char(0));
	auto outLen = 0;
	if (EVP_CipherUpdate(
			ctx,
			reinterpret_cast<uchar*>(out.data()),
			&outLen,
			reinterpret_cast<const uchar*>(input.constData()),
			input.size()) != 1) {
		return QByteArray();
	}
	auto finalLen = 0;
	if (EVP_CipherFinal_ex(
			ctx,
			reinterpret_cast<uchar*>(out.data()) + outLen,
			&finalLen) != 1) {
		return QByteArray(); // wrong password / corrupted padding on decrypt
	}
	out.resize(outLen + finalLen);
	return out;
}

[[nodiscard]] QByteArray RandomBytes(int size) {
	auto out = QByteArray(size, char(0));
	if (RAND_bytes(reinterpret_cast<uchar*>(out.data()), size) != 1) {
		return QByteArray();
	}
	return out;
}

// Walks the R,G,B channels (skipping alpha) of an RGBA8888 image in order.
// Returns a pointer to the channel byte for the given linear channel index.
[[nodiscard]] uchar *ChannelAt(QImage &image, qint64 channelIndex) {
	const auto width = image.width();
	const auto pixelIndex = channelIndex / 3;
	const auto channel = int(channelIndex % 3);
	const auto y = int(pixelIndex / width);
	const auto x = int(pixelIndex % width);
	return image.scanLine(y) + (x * 4) + channel;
}

[[nodiscard]] qint64 CapacityBits(const QImage &image) {
	return qint64(image.width()) * image.height() * 3;
}

void WriteBits(QImage &image, const QByteArray &stream) {
	const auto totalBits = qint64(stream.size()) * 8;
	for (auto i = qint64(0); i != totalBits; ++i) {
		const auto byte = uchar(stream[int(i / 8)]);
		const auto bit = (byte >> (7 - int(i % 8))) & 1;
		auto p = ChannelAt(image, i);
		*p = uchar((*p & 0xFE) | bit);
	}
}

[[nodiscard]] QByteArray ReadBytes(QImage &image, qint64 byteCount) {
	const auto totalBits = byteCount * 8;
	if (totalBits > CapacityBits(image)) {
		return QByteArray();
	}
	auto out = QByteArray(int(byteCount), char(0));
	for (auto i = qint64(0); i != totalBits; ++i) {
		const auto bit = (*ChannelAt(image, i)) & 1;
		out[int(i / 8)] = char(uchar(out[int(i / 8)]) | (bit << (7 - int(i % 8))));
	}
	return out;
}

[[nodiscard]] QImage ToRgba(const QImage &image) {
	return image.convertToFormat(QImage::Format_RGBA8888);
}

} // namespace

QByteArray Embed(
		const QImage &carrier,
		const QString &message,
		const QString &password) {
	const auto plain = message.toUtf8();
	if (plain.isEmpty() || plain.size() > kMaxMessageBytes || carrier.isNull()) {
		return QByteArray();
	}

	const auto encrypt = !password.isEmpty();
	const auto sha = QCryptographicHash::hash(plain, QCryptographicHash::Sha256);

	auto payload = plain;
	auto salt = QByteArray();
	auto iv = QByteArray();
	if (encrypt) {
		salt = RandomBytes(kSaltSize);
		iv = RandomBytes(kIvSize);
		const auto key = DeriveKey(password, salt);
		if (salt.isEmpty() || iv.isEmpty() || key.isEmpty()) {
			return QByteArray();
		}
		payload = AesCbc(true, plain, key, iv);
		if (payload.isEmpty()) {
			return QByteArray();
		}
	}

	auto frame = QByteArray();
	frame.append(kMagic, 4);
	frame.append(char(kVersion));
	frame.append(char(encrypt ? kFlagEncrypted : 0));
	if (encrypt) {
		frame.append(salt);
		frame.append(iv);
	}
	frame.append(sha);
	frame.append(payload);

	auto stream = U32BE(quint32(frame.size()));
	stream.append(frame);

	auto image = ToRgba(carrier);
	if (qint64(stream.size()) * 8 > CapacityBits(image)) {
		return QByteArray(); // image too small for this message
	}
	WriteBits(image, stream);

	auto out = QByteArray();
	auto buffer = QBuffer(&out);
	buffer.open(QIODevice::WriteOnly);
	if (!image.save(&buffer, "PNG")) {
		return QByteArray();
	}
	return out;
}

bool HasPayload(const QImage &source) {
	if (source.isNull()) {
		return false;
	}
	auto image = ToRgba(source);
	const auto head = ReadBytes(image, kLengthPrefixBytes + 4);
	if (head.size() < kLengthPrefixBytes + 4) {
		return false;
	}
	const auto frameLen = ReadU32BE(reinterpret_cast<const uchar*>(head.constData()));
	if (frameLen < 4 + 1 + 1 + kShaSize
		|| qint64(kLengthPrefixBytes + frameLen) * 8 > CapacityBits(image)) {
		return false;
	}
	return std::memcmp(head.constData() + kLengthPrefixBytes, kMagic, 4) == 0;
}

Result Extract(const QImage &source, const QString &password) {
	auto result = Result();
	if (source.isNull()) {
		return result;
	}
	auto image = ToRgba(source);

	const auto prefix = ReadBytes(image, kLengthPrefixBytes);
	if (prefix.size() < kLengthPrefixBytes) {
		return result;
	}
	const auto frameLen = ReadU32BE(reinterpret_cast<const uchar*>(prefix.constData()));
	if (frameLen < 4 + 1 + 1 + kShaSize
		|| qint64(kLengthPrefixBytes + frameLen) * 8 > CapacityBits(image)) {
		return result;
	}

	const auto full = ReadBytes(image, kLengthPrefixBytes + frameLen);
	if (full.size() < int(kLengthPrefixBytes + frameLen)) {
		return result;
	}
	const auto frame = full.mid(kLengthPrefixBytes);
	if (frame.size() < 4 || std::memcmp(frame.constData(), kMagic, 4) != 0) {
		return result;
	}
	result.present = true;

	auto offset = 4;
	const auto version = uchar(frame[offset++]);
	const auto flags = uchar(frame[offset++]);
	Q_UNUSED(version);
	result.encrypted = (flags & kFlagEncrypted);

	auto salt = QByteArray();
	auto iv = QByteArray();
	if (result.encrypted) {
		if (frame.size() < offset + kSaltSize + kIvSize + kShaSize) {
			return result;
		}
		salt = frame.mid(offset, kSaltSize);
		offset += kSaltSize;
		iv = frame.mid(offset, kIvSize);
		offset += kIvSize;
	}
	if (frame.size() < offset + kShaSize) {
		return result;
	}
	const auto sha = frame.mid(offset, kShaSize);
	offset += kShaSize;
	const auto payload = frame.mid(offset);

	auto plain = payload;
	if (result.encrypted) {
		if (password.isEmpty()) {
			return result; // present + encrypted, but no password yet
		}
		const auto key = DeriveKey(password, salt);
		if (key.isEmpty()) {
			return result;
		}
		plain = AesCbc(false, payload, key, iv);
		if (plain.isEmpty()) {
			return result; // wrong password / corrupted
		}
	}

	if (QCryptographicHash::hash(plain, QCryptographicHash::Sha256) != sha) {
		return result; // integrity check failed (wrong password or corruption)
	}

	result.ok = true;
	result.message = QString::fromUtf8(plain);
	return result;
}

} // namespace FurryStego
