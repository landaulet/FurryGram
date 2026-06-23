// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
//
// Modified as part of FurryGram, 2026.
#include "ayu/ui/settings/settings_main.h"

#include "settings/sections/settings_main.h"
#include "lang_auto.h"
#include "ayu/ayu_settings.h"
#include "ayu/ui/ayu_logo.h"
#include "ayu/ui/settings/settings_appearance.h"
#include "ayu/ui/settings/settings_ayu.h"
#include "ayu/ui/settings/settings_chats.h"
#include "ayu/ui/settings/settings_filters.h"
#include "ayu/ui/settings/settings_general.h"
#include "ayu/ui/settings/settings_other.h"
#include "ayu/ui/settings/settings_scripts.h"
#include "core/version.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "styles/style_ayu_settings.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "ui/painter.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "window/window_session_controller_link_info.h"

#include <QDesktopServices>

namespace Settings {

using namespace Builder;

namespace {

void BuildHeader(SectionBuilder &builder) {
	builder.add([](const WidgetContext &ctx) -> SectionBuilder::WidgetToAdd {
		auto widget = object_ptr<Ui::RpWidget>(ctx.container);
		const auto raw = widget.data();

		const auto logoSize = st::furryHeaderLogoSize;
		const auto logo = Ui::CreateChild<Ui::RpWidget>(raw);
		logo->resize(logoSize, logoSize);
		logo->paintRequest(
		) | rpl::on_next([=] {
			auto p = QPainter(logo);
			const auto image = AyuAssets::currentAppLogoPad();
			if (!image.isNull()) {
				const auto scaled = image.scaled(
					logoSize * style::DevicePixelRatio(),
					logoSize * style::DevicePixelRatio(),
					Qt::KeepAspectRatio,
					Qt::SmoothTransformation);
				p.drawImage(QRect(0, 0, logoSize, logoSize), scaled);
			}
		}, logo->lifetime());

		const auto title = Ui::CreateChild<Ui::FlatLabel>(
			raw,
			rpl::single(QString("FurryGram Desktop")),
			st::furryHeaderTitle);
		const auto version = Ui::CreateChild<Ui::FlatLabel>(
			raw,
			rpl::single(QString("v") + QString::fromLatin1(AppVersionStr)),
			st::furryHeaderVersion);

		raw->widthValue(
		) | rpl::on_next([=](int width) {
			const auto pad = st::furryHeaderPadding;
			const auto gap = st::furryHeaderGap;
			const auto vgap = st::furryHeaderTextGap;
			const auto textLeft = pad.left() + logoSize + gap;
			const auto textWidth = std::max(0, width - textLeft - pad.right());

			title->resizeToWidth(textWidth);
			version->resizeToWidth(textWidth);

			const auto textHeight = title->height() + vgap + version->height();
			const auto content = std::max(logoSize, textHeight);
			const auto height = pad.top() + content + pad.bottom();
			raw->resize(width, height);

			logo->move(pad.left(), pad.top() + (content - logoSize) / 2);

			auto y = pad.top() + (content - textHeight) / 2;
			title->move(textLeft, y);
			y += title->height() + vgap;
			version->move(textLeft, y);
		}, raw->lifetime());

		return { .widget = std::move(widget) };
	});
}

void BuildCategories(SectionBuilder &builder) {
	builder.addSkip();
	builder.addSkip();
	builder.addDivider();
	builder.addSkip();

	builder.addSubsectionTitle(tr::ayu_CategoriesHeader());

	builder.addSectionButton({
		.title = rpl::single(QString("FurryGram")),
		.targetSection = AyuGhost::Id(),
		.icon = { &st::menuIconGroupReactions },
	});
	builder.addSectionButton({
		.title = rpl::single(QString("Scripts")),
		.targetSection = AyuScripts::Id(),
		.icon = { &st::menuIconEdit },
	});
	builder.addSectionButton({
		.title = tr::ayu_CategoryFilters(),
		.targetSection = AyuFilters::Id(),
		.icon = { &st::menuIconTagFilter },
	});
	builder.addSectionButton({
		.title = tr::ayu_CategoryGeneral(),
		.targetSection = AyuGeneral::Id(),
		.icon = { &st::menuIconShowAll },
	});
	builder.addSectionButton({
		.title = tr::ayu_CategoryAppearance(),
		.targetSection = AyuAppearance::Id(),
		.icon = { &st::menuIconPalette },
	});
	builder.addSectionButton({
		.title = tr::ayu_CategoryChats(),
		.targetSection = AyuChats::Id(),
		.icon = { &st::menuIconChatBubble },
	});
	builder.addSectionButton({
		.title = tr::ayu_CategoryOther(),
		.targetSection = AyuOther::Id(),
		.icon = { &st::menuIconFave },
	});
}

void BuildLinks(SectionBuilder &builder) {
	builder.addSkip();
	builder.addDivider();
	builder.addSkip();

	builder.addSubsectionTitle(tr::ayu_LinksHeader());

	const auto controller = builder.controller();

	builder.addButton({
		.id = u"ayu/channel"_q,
		.title = tr::ayu_LinksChannel(),
		.icon = { &st::menuIconChannel },
		.label = rpl::single(QString("@FurryGramReleases")),
		.onClick = [=] {
			controller->showPeerByLink(Window::PeerByLinkInfo{
				.usernameOrId = QString("FurryGramReleases"),
			});
		},
	});
	builder.addButton({
		.id = u"ayu/crowdin"_q,
		.title = tr::ayu_LinksTranslate(),
		.icon = { &st::menuIconTranslate },
		.label = rpl::single(QString("GitHub")),
		.onClick = [=] {
			QDesktopServices::openUrl(
				QString("https://github.com/landaulet/FurryGram-Languages"));
		},
	});
	builder.addButton({
		.id = u"ayu/website"_q,
		.title = tr::ayu_LinksDocumentation(),
		.icon = { &st::menuIconIpAddress },
		.label = rpl::single(QString("landaulet.github.io/FurryGram-docs")),
		.onClick = [=] {
			QDesktopServices::openUrl(
				QString("https://landaulet.github.io/FurryGram-docs/"));
		},
	});

	builder.addSkip();
}

const auto kMeta = BuildHelper({
	.id = AyuMain::Id(),
	.parentId = MainId(),
	.title = &tr::ayu_AyuPreferences,
	.icon = &st::menuIconPremium,
}, [](SectionBuilder &builder) {
	BuildHeader(builder);
	BuildCategories(builder);
	BuildLinks(builder);
});

} // namespace

rpl::producer<QString> AyuMain::title() {
	return rpl::single(QString(""));
}

AyuMain::AyuMain(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

void AyuMain::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

Type AyuMainId() {
	return AyuMain::Id();
}

} // namespace Settings
