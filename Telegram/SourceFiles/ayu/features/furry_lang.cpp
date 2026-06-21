// FurryGram: tiny built-in localization helper for FurryGram-only strings.
#include "ayu/features/furry_lang.h"

#include "lang/lang_instance.h"

namespace FurryLang {

bool IsRussian() {
	// Match the official "ru" pack and any Russian-based pack (e.g. a custom
	// pack whose base language is Russian).
	const auto isRu = [](const QString &id) {
		return id == u"ru"_q || id.startsWith(u"ru-"_q);
	};
	return isRu(Lang::GetInstance().id())
		|| isRu(Lang::GetInstance().baseId());
}

QString Pick(const QString &en, const QString &ru) {
	return IsRussian() ? ru : en;
}

} // namespace FurryLang
