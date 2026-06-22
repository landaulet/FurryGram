// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
//
// Modified as part of FurryGram, 2026.
#include "ayu/ui/utils/ayu_profile_values.h"

#include "ayu/ayu_settings.h"
#include "ayu/utils/telegram_helpers.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "base/flat_map.h"
#include "lang/lang_text_entity.h"

constexpr auto kMaxChannelId = -1000000000000;

QString IDString(const not_null<PeerData*> peer) {
	auto resultId = QString::number(getBareID(peer));

	const auto &settings = AyuSettings::getInstance();
	if (settings.showPeerId() == PeerIdDisplay::BotApi) {
		if (peer->isChannel()) {
			resultId = QString::number(peerToChannel(peer->id).bare - kMaxChannelId).prepend("-");
		} else if (peer->isChat()) {
			resultId = resultId.prepend("-");
		}
	}

	return resultId;
}

QString IDString(MsgId topicRootId) {
	return QString::number(topicRootId.bare);
}

rpl::producer<TextWithEntities> IDValue(not_null<PeerData*> peer) {
	return AyuSettings::getInstance().showPeerIdValue(
	) | rpl::map([=](PeerIdDisplay display) {
		return (display == PeerIdDisplay::Hidden)
			? TextWithEntities()
			: tr::marked(IDString(peer));
	});
}

rpl::producer<TextWithEntities> IDValue(MsgId topicRootId) {
	return AyuSettings::getInstance().showPeerIdValue(
	) | rpl::map([=](PeerIdDisplay display) {
		return (display == PeerIdDisplay::Hidden)
			? TextWithEntities()
			: tr::marked(IDString(topicRootId));
	});
}

rpl::producer<TextWithEntities> RegistrationDateValue(not_null<UserData*> user) {
	// Per-user variable kept for the app lifetime, so the async fetch can update
	// it safely even if the profile widget that subscribed is already gone.
	static auto cache = base::flat_map<
		ID,
		std::unique_ptr<rpl::variable<TextWithEntities>>>();
	const auto wrap = [](const QString &s) {
		return s.isEmpty() ? TextWithEntities() : TextWithEntities{ s };
	};
	const auto id = getBareID(user);
	auto it = cache.find(id);
	if (it == cache.end()) {
		it = cache.emplace(
			id,
			std::make_unique<rpl::variable<TextWithEntities>>()).first;
		const auto raw = it->second.get();
		if (const auto cached = getCachedRegistrationDate(id)) {
			*raw = wrap(*cached);
		} else {
			getRegistrationDate(user, [=](TextWithEntities) {
				if (const auto now = getCachedRegistrationDate(id)) {
					*raw = wrap(*now);
				}
			});
		}
	}
	return it->second->value();
}
