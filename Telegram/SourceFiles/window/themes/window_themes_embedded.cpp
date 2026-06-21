/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/themes/window_themes_embedded.h"

#include "base/platform/base_platform_info.h"
#include "lang/lang_keys.h"
#include "storage/serialize_common.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "ui/style/style_palette_colorizer.h"
#include "window/themes/window_theme.h"

#include <QtGui/QGuiApplication>
#include <QtGui/QPalette>

// AyuGram includes
#include "ayu/features/message_shot/message_shot.h"


namespace Window {
namespace Theme {
namespace {

constexpr auto kMaxAccentColors = 3;
constexpr auto kDayBaseFile = ":/gui/day-custom-base.tdesktop-theme"_cs;
constexpr auto kNightBaseFile = ":/gui/night-custom-base.tdesktop-theme"_cs;

const auto kColorizeIgnoredKeys = base::flat_set<QLatin1String>{ {
	qstr("boxTextFgGood"),
	qstr("boxTextFgError"),
	qstr("callIconFg"),
	qstr("historyPeer1NameFg"),
	qstr("historyPeer1NameFgSelected"),
	qstr("historyPeer1UserpicBg"),
	qstr("historyPeer2NameFg"),
	qstr("historyPeer2NameFgSelected"),
	qstr("historyPeer2UserpicBg"),
	qstr("historyPeer3NameFg"),
	qstr("historyPeer3NameFgSelected"),
	qstr("historyPeer3UserpicBg"),
	qstr("historyPeer4NameFg"),
	qstr("historyPeer4NameFgSelected"),
	qstr("historyPeer4UserpicBg"),
	qstr("historyPeer5NameFg"),
	qstr("historyPeer5NameFgSelected"),
	qstr("historyPeer5UserpicBg"),
	qstr("historyPeer6NameFg"),
	qstr("historyPeer6NameFgSelected"),
	qstr("historyPeer6UserpicBg"),
	qstr("historyPeer7NameFg"),
	qstr("historyPeer7NameFgSelected"),
	qstr("historyPeer7UserpicBg"),
	qstr("historyPeer8NameFg"),
	qstr("historyPeer8NameFgSelected"),
	qstr("historyPeer8UserpicBg"),
	qstr("historyPeer1UserpicBg2"),
	qstr("historyPeer2UserpicBg2"),
	qstr("historyPeer3UserpicBg2"),
	qstr("historyPeer4UserpicBg2"),
	qstr("historyPeer5UserpicBg2"),
	qstr("historyPeer6UserpicBg2"),
	qstr("historyPeer7UserpicBg2"),
	qstr("historyPeer8UserpicBg2"),
	qstr("msgFile1Bg"),
	qstr("msgFile1BgDark"),
	qstr("msgFile1BgOver"),
	qstr("msgFile1BgSelected"),
	qstr("msgFile2Bg"),
	qstr("msgFile2BgDark"),
	qstr("msgFile2BgOver"),
	qstr("msgFile2BgSelected"),
	qstr("msgFile3Bg"),
	qstr("msgFile3BgDark"),
	qstr("msgFile3BgOver"),
	qstr("msgFile3BgSelected"),
	qstr("msgFile4Bg"),
	qstr("msgFile4BgDark"),
	qstr("msgFile4BgOver"),
	qstr("msgFile4BgSelected"),
	qstr("mediaviewFileRedCornerFg"),
	qstr("mediaviewFileYellowCornerFg"),
	qstr("mediaviewFileGreenCornerFg"),
	qstr("mediaviewFileBlueCornerFg"),
	qstr("settingsIconBg1"),
	qstr("settingsIconBg2"),
	qstr("settingsIconBg3"),
	qstr("settingsIconBg4"),
	qstr("settingsIconBg5"),
	qstr("settingsIconBg6"),
	qstr("settingsIconBg8"),
	qstr("settingsIconBgArchive"),
	qstr("premiumButtonBg1"),
	qstr("premiumButtonBg2"),
	qstr("premiumButtonBg3"),
	qstr("premiumIconBg1"),
	qstr("premiumIconBg2"),
} };

// FurryGram night-based recolor themes: their .tdesktop-theme file is a copy
// of the stock night theme, recolored at load-time to the designed accent
// (the same mechanism Telegram uses for its accent variants). The source hue
// is the stock night accent; the target is the scheme's accentColor.
constexpr auto kFurryNightBaseAccent = "5288c1";

[[nodiscard]] bool IsFurryRecolorType(EmbeddedType type) {
	return (type == EmbeddedType::FurryViolet)
		|| (type == EmbeddedType::FurrySunset)
		|| (type == EmbeddedType::FurryForest);
}

style::colorizer::Color cColor(std::string_view hex) {
	const auto q = style::ColorFromHex(hex);
	auto hue = int();
	auto saturation = int();
	auto value = int();
	q.getHsv(&hue, &saturation, &value);
	return style::colorizer::Color{ hue, saturation, value };
}

} // namespace

bool IsFurryTheme(EmbeddedType type) {
	return (type == EmbeddedType::FurryAero)
		|| (type == EmbeddedType::FurryViolet)
		|| (type == EmbeddedType::FurrySunset)
		|| (type == EmbeddedType::FurryForest);
}

style::colorizer ColorizerFrom(
		const EmbeddedScheme &scheme,
		const QColor &color) {
	using Color = style::colorizer::Color;
	using Pair = std::pair<Color, Color>;

	auto result = style::colorizer();
	result.ignoreKeys = kColorizeIgnoredKeys;
	result.hueThreshold = 15;
	scheme.accentColor.getHsv(
		&result.was.hue,
		&result.was.saturation,
		&result.was.value);
	color.getHsv(
		&result.now.hue,
		&result.now.saturation,
		&result.now.value);
	switch (scheme.type) {
	case EmbeddedType::Default:
		result.lightnessMax = 160;
		break;
	case EmbeddedType::DayBlue:
		result.lightnessMax = 160;
		break;
	case EmbeddedType::FurryAero: // ocean-blue dark theme: treat like Night
	case EmbeddedType::FurryViolet: // night-based recolors, treat like Night
	case EmbeddedType::FurrySunset:
	case EmbeddedType::FurryForest:
	case EmbeddedType::Night:
		result.keepContrast = base::flat_map<QLatin1String, Pair>{ {
			//{ qstr("windowFgActive"), Pair{ cColor("5288c1"), cColor("17212b") } }, // windowBgActive
			{ qstr("activeButtonFg"), Pair{ cColor("2f6ea5"), cColor("17212b") } }, // activeButtonBg
			{ qstr("profileVerifiedCheckFg"), Pair{ cColor("5288c1"), cColor("17212b") } }, // profileVerifiedCheckBg
			{ qstr("overviewCheckFgActive"), Pair{ cColor("5288c1"), cColor("17212b") } }, // overviewCheckBgActive
			{ qstr("historyFileInIconFg"), Pair{ cColor("3f96d0"), cColor("182533") } }, // msgFileInBg, msgInBg
			{ qstr("historyFileInIconFgSelected"), Pair{ cColor("6ab4f4"), cColor("2e70a5") } }, // msgFileInBgSelected, msgInBgSelected
			{ qstr("historyFileInRadialFg"), Pair{ cColor("3f96d0"), cColor("182533") } }, // msgFileInBg, msgInBg
			{ qstr("historyFileInRadialFgSelected"), Pair{ cColor("6ab4f4"), cColor("2e70a5") } }, // msgFileInBgSelected, msgInBgSelected
			{ qstr("historyFileOutIconFg"), Pair{ cColor("4c9ce2"), cColor("2b5278") } }, // msgFileOutBg, msgOutBg
			{ qstr("historyFileOutIconFgSelected"), Pair{ cColor("58abf3"), cColor("2e70a5") } }, // msgFileOutBgSelected, msgOutBgSelected
			{ qstr("historyFileOutRadialFg"), Pair{ cColor("4c9ce2"), cColor("2b5278") } }, // msgFileOutBg, msgOutBg
			{ qstr("historyFileOutRadialFgSelected"), Pair{ cColor("58abf3"), cColor("2e70a5") } }, // msgFileOutBgSelected, msgOutBgSelected
		} };
		result.lightnessMin = 64;
		break;
	case EmbeddedType::NightGreen:
		result.keepContrast = base::flat_map<QLatin1String, Pair>{ {
			//{ qstr("windowFgActive"), Pair{ cColor("3fc1b0"), cColor("282e33") } }, // windowBgActive, windowBg
			{ qstr("activeButtonFg"), Pair{ cColor("2da192"), cColor("282e33") } }, // activeButtonBg, windowBg
			{ qstr("profileVerifiedCheckFg"), Pair{ cColor("3fc1b0"), cColor("282e33") } }, // profileVerifiedCheckBg, windowBg
			{ qstr("overviewCheckFgActive"), Pair{ cColor("3fc1b0"), cColor("282e33") } }, // overviewCheckBgActive
			// callIconFg is used not only over callAnswerBg,
			// so this contrast-forcing breaks other buttons.
			//{ qstr("callIconFg"), Pair{ cColor("5ad1c1"), cColor("1b1f23") } }, // callAnswerBg, callBgOpaque
		} };
		result.lightnessMin = 64;
		break;
	}
	const auto nowLightness = color.lightness();
	const auto limitedLightness = std::clamp(
		nowLightness,
		result.lightnessMin,
		result.lightnessMax);
	if (limitedLightness != nowLightness) {
		QColor::fromHsl(
			color.hslHue(),
			color.hslSaturation(),
			limitedLightness).getHsv(
				&result.now.hue,
				&result.now.saturation,
				&result.now.value);
	}
	return result;
}

std::optional<QColor> SystemAccentColor() {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	if (Platform::IsWindows() && Platform::IsWindows8OrGreater()) {
		return std::nullopt;
	}
#endif // Qt < 6.0.0
	const auto accent = QPalette().color(QPalette::Highlight);
	return accent.isValid() ? std::make_optional(accent) : std::nullopt;
}

style::colorizer ColorizerForTheme(const QString &absolutePath) {
	if (!IsEmbeddedTheme(absolutePath)) {
		return {};
	}
	const auto schemes = EmbeddedThemes();
	const auto i = ranges::find(
		schemes,
		absolutePath,
		&EmbeddedScheme::path);
	if (i == end(schemes)) {
		return {};
	}
	const auto &settings = Core::App().settings();
	if (settings.systemAccentColorEnabled()) {
		if (const auto accent = SystemAccentColor()) {
			return ColorizerFrom(*i, *accent);
		}
	}
	const auto &colors = settings.themesAccentColors();
	if (const auto accent = AyuFeatures::MessageShot::isChoosingTheme() ? AyuFeatures::MessageShot::getSelectedColorFromDefault() : colors.get(i->type)) {
		return ColorizerFrom(*i, *accent);
	}
	// FurryGram night-recolor themes: with no user-chosen accent, recolor the
	// stock night base from its native accent to the scheme's designed accent.
	if (IsFurryRecolorType(i->type)) {
		auto base = *i;
		base.accentColor = style::ColorFromHex(kFurryNightBaseAccent);
		return ColorizerFrom(base, i->accentColor);
	}
	return {};
}

void Colorize(EmbeddedScheme &scheme, const style::colorizer &colorizer) {
	const auto colors = {
		&EmbeddedScheme::background,
		&EmbeddedScheme::sent,
		&EmbeddedScheme::received,
		&EmbeddedScheme::radiobuttonActive,
		&EmbeddedScheme::radiobuttonInactive
	};
	for (const auto color : colors) {
		if (const auto changed = style::colorize(scheme.*color, colorizer)) {
			scheme.*color = changed->toRgb();
		}
	}
}

std::vector<EmbeddedScheme> EmbeddedThemes() {
	const auto qColor = [](auto hex) {
		return style::ColorFromHex(hex);
	};
	const auto name = [](auto key) {
		return rpl::deferred([=] { return key(); });
	};
	return {
		EmbeddedScheme{
			EmbeddedType::Default,
			qColor("9bd494"),
			qColor("eaffdc"),
			qColor("ffffff"),
			qColor("eaffdc"),
			qColor("ffffff"),
			name(tr::lng_settings_theme_classic),
			QString(),
			qColor("40a7e3")
		},
		EmbeddedScheme{
			EmbeddedType::DayBlue,
			qColor("7ec4ea"),
			qColor("d7f0ff"),
			qColor("ffffff"),
			qColor("d7f0ff"),
			qColor("ffffff"),
			name(tr::lng_settings_theme_day),
			":/gui/day-blue.tdesktop-theme",
			qColor("40a7e3")
		},
		EmbeddedScheme{
			EmbeddedType::Night,
			qColor("485761"),
			qColor("5ca7d4"),
			qColor("6b808d"),
			qColor("6b808d"),
			qColor("5ca7d4"),
			name(tr::lng_settings_theme_tinted),
			":/gui/night.tdesktop-theme",
			qColor("5288c1")
		},
		EmbeddedScheme{
			EmbeddedType::NightGreen,
			qColor("485761"),
			qColor("6b808d"),
			qColor("6b808d"),
			qColor("6b808d"),
			qColor("75bfb5"),
			name(tr::lng_settings_theme_night),
			":/gui/night-green.tdesktop-theme",
			qColor("3fc1b0")
		},
		EmbeddedScheme{
			EmbeddedType::FurryAero,
			qColor("112334"), // background preview (deep ocean)
			qColor("1b4a87"), // sent
			qColor("16314a"), // received
			qColor("6b808d"),
			qColor("5a9ff0"),
			rpl::single(QString("FurryGram Aero")),
			":/gui/furrygram-aero.tdesktop-theme",
			qColor("2272d4") // ocean blue accent
		},
		EmbeddedScheme{
			EmbeddedType::FurryViolet,
			qColor("1d1530"), // background preview (deep violet)
			qColor("4a2d87"), // sent
			qColor("261a3a"), // received
			qColor("6b808d"),
			qColor("9a5af0"),
			rpl::single(QString("FurryGram Violet")),
			":/gui/furrygram-violet.tdesktop-theme",
			qColor("7a4fd0") // violet accent
		},
		EmbeddedScheme{
			EmbeddedType::FurrySunset,
			qColor("2e1a16"), // background preview (warm dusk)
			qColor("8a3d28"), // sent
			qColor("3a221a"), // received
			qColor("8d7b6b"),
			qColor("f0915a"),
			rpl::single(QString("FurryGram Sunset")),
			":/gui/furrygram-sunset.tdesktop-theme",
			qColor("e0734a") // warm orange accent
		},
		EmbeddedScheme{
			EmbeddedType::FurryForest,
			qColor("0f2622"), // background preview (deep teal-green)
			qColor("1b6b56"), // sent
			qColor("16312b"), // received
			qColor("6b8d83"),
			qColor("5af0c0"),
			rpl::single(QString("FurryGram Forest")),
			":/gui/furrygram-forest.tdesktop-theme",
			qColor("2faf8f") // teal-green accent
		},
	};
}

std::vector<QColor> DefaultAccentColors(EmbeddedType type) {
	const auto qColor = [](auto hex) {
		return style::ColorFromHex(hex);
	};
	switch (type) {
	case EmbeddedType::DayBlue:
		return {
			qColor("45bce7"),
			qColor("52b440"),
			qColor("d46c99"),
			qColor("df8a49"),
			qColor("9978c8"),
			qColor("c55245"),
			qColor("687b98"),
			qColor("dea922"),
		};
	case EmbeddedType::Default:
		return {
			qColor("45bce7"),
			qColor("52b440"),
			qColor("d46c99"),
			qColor("df8a49"),
			qColor("9978c8"),
			qColor("c55245"),
			qColor("687b98"),
			qColor("dea922"),
		};
	case EmbeddedType::Night:
		return {
			qColor("58bfe8"),
			qColor("466f42"),
			qColor("aa6084"),
			qColor("a46d3c"),
			qColor("917bbd"),
			qColor("ab5149"),
			qColor("697b97"),
			qColor("9b834b"),
		};
	case EmbeddedType::NightGreen:
		return {
			qColor("60a8e7"),
			qColor("4e9c57"),
			qColor("ca7896"),
			qColor("cc925c"),
			qColor("a58ed2"),
			qColor("d27570"),
			qColor("7b8799"),
			qColor("cbac67"),
		};
	case EmbeddedType::FurryAero:
	case EmbeddedType::FurryViolet:
	case EmbeddedType::FurrySunset:
	case EmbeddedType::FurryForest:
		return {}; // fixed designed themes, no alternate accent swatches
	}
	Unexpected("Type in Window::Theme::AccentColors.");
}

Fn<void(style::palette&)> PreparePaletteCallback(
		bool dark,
		std::optional<QColor> accent) {
	return [=](style::palette &palette) {
		using namespace Theme;
		const auto &embedded = EmbeddedThemes();
		const auto i = ranges::find(
			embedded,
			dark ? EmbeddedType::Night : EmbeddedType::Default,
			&EmbeddedScheme::type);
		Assert(i != end(embedded));
		const auto colorizer = accent
			? ColorizerFrom(*i, *accent)
			: style::colorizer();

		auto instance = Instance();
		const auto loaded = LoadFromFile(
			(dark ? kNightBaseFile : kDayBaseFile).utf16(),
			&instance,
			nullptr,
			nullptr,
			colorizer);
		Assert(loaded);
		palette.finalize();
		palette = instance.palette;
	};
}

Fn<void(style::palette&)> PrepareCurrentPaletteCallback() {
	return [=, data = style::main_palette::save()](style::palette &palette) {
		palette.load(data);
	};
}

QByteArray AccentColors::serialize() const {
	auto result = QByteArray();
	if (_data.empty()) {
		return result;
	}

	const auto count = _data.size();
	auto size = sizeof(qint32) * (count + 1)
		+ Serialize::colorSize() * count;
	result.reserve(size);

	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << qint32(_data.size());
	for (const auto &[type, color] : _data) {
		stream << static_cast<qint32>(type);
		Serialize::writeColor(stream, color);
	}
	stream.device()->close();

	return result;
}

bool AccentColors::setFromSerialized(const QByteArray &serialized) {
	if (serialized.isEmpty()) {
		_data.clear();
		return true;
	}
	auto copy = QByteArray(serialized);
	auto stream = QDataStream(&copy, QIODevice::ReadOnly);
	stream.setVersion(QDataStream::Qt_5_1);

	auto count = qint32();
	stream >> count;
	if (stream.status() != QDataStream::Ok) {
		return false;
	} else if (count <= 0 || count > kMaxAccentColors) {
		return false;
	}
	auto data = base::flat_map<EmbeddedType, QColor>();
	for (auto i = 0; i != count; ++i) {
		auto type = qint32();
		stream >> type;
		const auto color = Serialize::readColor(stream);
		const auto uncheckedType = static_cast<EmbeddedType>(type);
		switch (uncheckedType) {
		case EmbeddedType::Default:
		case EmbeddedType::DayBlue:
		case EmbeddedType::Night:
		case EmbeddedType::NightGreen:
		case EmbeddedType::FurryAero:
		case EmbeddedType::FurryViolet:
		case EmbeddedType::FurrySunset:
		case EmbeddedType::FurryForest:
			data.emplace(uncheckedType, color);
			break;
		default:
			return false;
		}
	}
	if (stream.status() != QDataStream::Ok) {
		return false;
	}
	_data = std::move(data);
	return true;
}

void AccentColors::set(EmbeddedType type, const QColor &value) {
	_data.emplace_or_assign(type, value);
}

void AccentColors::clear(EmbeddedType type) {
	_data.remove(type);
}

std::optional<QColor> AccentColors::get(EmbeddedType type) const {
	const auto i = _data.find(type);
	return (i != end(_data)) ? std::make_optional(i->second) : std::nullopt;
}

} // namespace Theme
} // namespace Window
