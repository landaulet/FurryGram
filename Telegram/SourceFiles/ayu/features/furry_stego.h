// FurryGram: image steganography (hide an encrypted message inside an image).
#pragma once

#include <QtGui/QImage>

namespace FurryStego {

// Sanity cap for the plaintext message.
inline constexpr auto kMaxMessageBytes = 64 * 1024;

// True if `image` carries a FurryGram stego payload (cheap magic check).
[[nodiscard]] bool HasPayload(const QImage &image);

// Embeds `message` (UTF-8) into a copy of `carrier` via LSB. If `password` is
// non-empty the message is AES-256-CBC encrypted (key derived via PBKDF2).
// Returns lossless PNG bytes, or an empty QByteArray on failure (image too
// small for the payload, empty message, or a crypto error).
[[nodiscard]] QByteArray Embed(
	const QImage &carrier,
	const QString &message,
	const QString &password);

struct Result {
	bool present = false;   // our payload was detected
	bool encrypted = false; // payload is encrypted (needs a password)
	bool ok = false;        // extracted (and decrypted) successfully
	QString message;        // the recovered text (valid only when ok)
};

// Extracts a payload from `image`. When the payload is encrypted and the
// password is empty/wrong, `present`/`encrypted` are set but `ok` is false so
// the UI can prompt for (or re-ask) the password.
[[nodiscard]] Result Extract(const QImage &image, const QString &password);

} // namespace FurryStego
