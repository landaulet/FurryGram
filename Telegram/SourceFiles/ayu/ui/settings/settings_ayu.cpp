// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/settings/settings_ayu.h"

#include "lang_auto.h"
#include "ayu/ayu_settings.h"
#include "ayu/features/furry_lang.h"
#include "ayu/features/ocr.h"
#include "ayu/features/voice_transcribe.h"
#include "ayu/features/media_clip.h"
#include "ayu/ui/ayu_userpic.h"
#include "ayu/ui/settings/ayu_builder.h"
#include "ayu/ui/settings/focus_exceptions_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/input_field.h"
#include "data/data_chat_filters.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "ayu/ui/settings/settings_ayu_utils.h"
#include "ayu/ui/settings/settings_main.h"
#include "boxes/peer_list_box.h"
#include "core/application.h"
#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "styles/style_ayu_icons.h"
#include "styles/style_ayu_styles.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_window.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/boxes/choose_date_time.h"
#include "base/unixtime.h"

#include <QtCore/QDateTime>
#include "ui/text/text.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/menu/menu_item_base.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

namespace Settings {

using namespace Builder;
using namespace AyuBuilder;

namespace {

struct GhostPickerState {
	rpl::variable<uint64> selectedUserId;
	base::unique_qptr<Ui::PopupMenu> menu;
	Ui::LinkButton *pickerButton = nullptr;
	Fn<void()> refreshCheckboxes;
};

struct AccountUserpicGeometry {
	QRect outer;
	QRect inner;
};

[[nodiscard]] AccountUserpicGeometry AccountUserpic(int height) {
	const auto line = st::mainMenuAccountLine;
	const auto skip = 2 * line + st::lineWidth;
	const auto full = st::mainMenuAccountSize + 2 * skip;
	const auto outer = QRect(
		st::defaultWhoRead.photoLeft
			+ (st::defaultWhoRead.photoSize - full) / 2,
		(height - full) / 2,
		full,
		full);
	return {
		.outer = outer,
		.inner = QRect(
			outer.x() + skip,
			outer.y() + skip,
			st::mainMenuAccountSize,
			st::mainMenuAccountSize),
	};
}

void PaintAccountOutline(Painter &p, QRect outer) {
	const auto line = st::mainMenuAccountLine;
	const auto shift = st::lineWidth + (line * 0.5);
	const auto rect = QRectF(outer).marginsRemoved(QMarginsF(
		shift,
		shift,
		shift,
		shift));
	auto hq = PainterHighQualityEnabler(p);
	auto pen = st::windowBgActive->p;
	pen.setWidthF(line);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	AyuUserpic::PaintShape(p, rect);
}

class AccountAction final : public Ui::Menu::ItemBase {
public:
	AccountAction(
		not_null<Ui::Menu::Menu*> parent,
		const style::Menu &st,
		UserData *user,
		bool active,
		Fn<void()> callback)
	: ItemBase(parent, st)
	, _dummyAction(Ui::CreateChild<QAction>(parent.get()))
	, _user(user)
	, _active(active)
	, _st(st)
	, _height(st::defaultWhoRead.photoSkip * 2 + st::defaultWhoRead.photoSize) {
		setAcceptBoth(true);
		fitToMenuWidth();

		_text.setText(_st.itemStyle, user->name());
		const auto goodWidth = st::defaultWhoRead.nameLeft
			+ _text.maxWidth()
			+ _st.itemPadding.right();
		setMinWidth(std::clamp(goodWidth, _st.widthMin, _st.widthMax));

		setActionTriggered(std::move(callback));

		paintRequest(
		) | rpl::on_next([=] {
			paint(Painter(this));
		}, lifetime());

		enableMouseSelecting();
	}

	not_null<QAction*> action() const override { return _dummyAction; }
	bool isEnabled() const override { return true; }

protected:
	int contentHeight() const override { return _height; }

private:
	void paint(Painter &&p) {
		const auto selected = isSelected();
		if (selected && _st.itemBgOver->c.alpha() < 255) {
			p.fillRect(0, 0, width(), _height, _st.itemBg);
		}
		const auto bg = selected ? _st.itemBgOver : _st.itemBg;
		p.fillRect(0, 0, width(), _height, bg);
		if (isEnabled()) {
			paintRipple(p, 0, 0);
		}

		const auto userpic = AccountUserpic(_height);
		_user->paintUserpicLeft(
			p,
			_userpicView,
			userpic.inner.x(),
			userpic.inner.y(),
			width(),
			userpic.inner.width());
		if (_active) {
			PaintAccountOutline(p, userpic.outer);
		}

		p.setPen(selected ? _st.itemFgOver : _st.itemFg);
		_text.drawLeftElided(
			p,
			st::defaultWhoRead.nameLeft,
			(_height - _st.itemStyle.font->height) / 2,
			width() - st::defaultWhoRead.nameLeft - _st.itemPadding.right(),
			width());
	}

	const not_null<QAction*> _dummyAction;
	UserData *_user = nullptr;
	mutable Ui::PeerUserpicView _userpicView;
	const bool _active = false;
	const style::Menu &_st;
	Ui::Text::String _text;
	const int _height;
};

class GlobalAction final : public Ui::Menu::ItemBase {
public:
	GlobalAction(
		not_null<Ui::Menu::Menu*> parent,
		const style::Menu &st,
		const QString &text,
		bool active,
		Fn<void()> callback)
	: ItemBase(parent, st)
	, _dummyAction(Ui::CreateChild<QAction>(parent.get()))
	, _active(active)
	, _st(st)
	, _height(st::defaultWhoRead.photoSkip * 2 + st::defaultWhoRead.photoSize) {
		setAcceptBoth(true);
		fitToMenuWidth();

		_text.setText(_st.itemStyle, text);
		const auto goodWidth = st::defaultWhoRead.nameLeft
			+ _text.maxWidth()
			+ _st.itemPadding.right();
		setMinWidth(std::clamp(goodWidth, _st.widthMin, _st.widthMax));

		setActionTriggered(std::move(callback));

		paintRequest(
		) | rpl::on_next([=] {
			paint(Painter(this));
		}, lifetime());

		enableMouseSelecting();
	}

	not_null<QAction*> action() const override { return _dummyAction; }
	bool isEnabled() const override { return true; }

protected:
	int contentHeight() const override { return _height; }

private:
	void paint(Painter &&p) {
		const auto selected = isSelected();
		if (selected && _st.itemBgOver->c.alpha() < 255) {
			p.fillRect(0, 0, width(), _height, _st.itemBg);
		}
		const auto bg = selected ? _st.itemBgOver : _st.itemBg;
		p.fillRect(0, 0, width(), _height, bg);
		if (isEnabled()) {
			paintRipple(p, 0, 0);
		}

		const auto userpic = AccountUserpic(_height);
		{
			auto hq = PainterHighQualityEnabler(p);
			auto rect = QRectF(userpic.inner);
			auto gradient = QLinearGradient(rect.topLeft(), rect.bottomLeft());
			gradient.setStops({
				{ 0., st::historyPeer5UserpicBg->c },
				{ 1., st::historyPeer5UserpicBg2->c },
			});
			p.setPen(Qt::NoPen);
			p.setBrush(gradient);
			AyuUserpic::PaintShape(p, rect);
		}
		{
			auto hq = PainterHighQualityEnabler(p);
			p.drawImage(
				userpic.inner,
				st::ayuGhostModeGlobalIcon.instance(st::historyPeerUserpicFg->c));
		}
		if (_active) {
			PaintAccountOutline(p, userpic.outer);
		}

		p.setPen(selected ? _st.itemFgOver : _st.itemFg);
		_text.drawLeftElided(
			p,
			st::defaultWhoRead.nameLeft,
			(_height - _st.itemStyle.font->height) / 2,
			width() - st::defaultWhoRead.nameLeft - _st.itemPadding.right(),
			width());
	}

	const not_null<QAction*> _dummyAction;
	const bool _active = false;
	const style::Menu &_st;
	Ui::Text::String _text;
	const int _height;
};

QString GetAccountName(uint64 userId) {
	for (const auto &account : Core::App().domain().orderedAccounts()) {
		if (account->sessionExists()
			&& account->session().userId().bare == userId) {
			return account->session().user()->name();
		}
	}
	return FurryLang::Pick(u"Unknown"_q, QString::fromUtf8("\xD0\x9D\xD0\xB5\xD0\xB8\xD0\xB7\xD0\xB2\xD0\xB5\xD1\x81\xD1\x82\xD0\xBD\xD0\xBE"));
}

QString PickerLabel(uint64 userId) {
	return (userId == 0)
		? tr::ayu_GhostModeGlobalSettings(tr::now)
		: GetAccountName(userId);
}

void selectGhostProfile(GhostPickerState *state, uint64 userId) {
	if (state->selectedUserId.current() == userId) {
		return;
	}

	auto wasGlobal = (state->selectedUserId.current() == 0);
	auto nowGlobal = (userId == 0);

	AyuSettings::getInstance().setUseGlobalGhostMode(nowGlobal);

	state->selectedUserId = userId;

	state->pickerButton->setText(PickerLabel(userId));

	state->refreshCheckboxes();

	if (wasGlobal != nowGlobal) {
		Ui::Toast::Show(nowGlobal
			? tr::ayu_GhostModeSwitchedToGlobalSettings(tr::now)
			: tr::ayu_GhostModeSwitchedToIndividualSettings(tr::now));
	}
}

void BuildGhostEssentials(SectionBuilder &builder) {
	builder.add([](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			const auto container = wctx.container;
			const auto controller = wctx.controller;

			auto activeCount = 0;
			for (const auto &account : Core::App().domain().orderedAccounts()) {
				if (account->sessionExists()) {
					++activeCount;
				}
			}

			if (activeCount <= 1 && !AyuSettings::getInstance().useGlobalGhostMode()) {
				auto userId = controller->session().userId().bare;
				auto &src = AyuSettings::ghost(userId);
				auto &dst = AyuSettings::ghost(0);
				dst.setSendReadMessages(src.sendReadMessages());
				dst.setSendReadStories(src.sendReadStories());
				dst.setSendOnlinePackets(src.sendOnlinePackets());
				dst.setSendUploadProgress(src.sendUploadProgress());
				dst.setSendOfflinePacketAfterOnline(src.sendOfflinePacketAfterOnline());
				dst.setMarkReadAfterAction(src.markReadAfterAction());
				dst.setUseScheduledMessages(src.useScheduledMessages());
				dst.setSendWithoutSound(src.sendWithoutSound());
				dst.setSuggestGhostModeBeforeViewingStory(src.suggestGhostModeBeforeViewingStory());
				dst.setSendReadMessagesLocked(src.sendReadMessagesLocked());
				dst.setSendReadStoriesLocked(src.sendReadStoriesLocked());
				dst.setSendOnlinePacketsLocked(src.sendOnlinePacketsLocked());
				dst.setSendUploadProgressLocked(src.sendUploadProgressLocked());
				dst.setSendOfflinePacketAfterOnlineLocked(src.sendOfflinePacketAfterOnlineLocked());
				AyuSettings::getInstance().setUseGlobalGhostMode(true);
			}

			const auto isGlobal = AyuSettings::getInstance().useGlobalGhostMode();
			auto initialUserId = isGlobal
				? uint64(0)
				: controller->session().userId().bare;

			const auto state = container->lifetime().make_state<GhostPickerState>();
			state->selectedUserId = initialUserId;

			const auto title = AddSubsectionTitle(container, tr::ayu_GhostEssentialsHeader());

			const auto pickerButton = Ui::CreateChild<Ui::LinkButton>(
				container.get(),
				PickerLabel(initialUserId),
				st::ghostPickerButton);
			state->pickerButton = pickerButton;

			const auto arrow = Ui::CreateChild<Ui::AbstractButton>(container.get());
			{
				const auto &icon = st::ghostPickerArrow;
				arrow->resize(icon.size());
				arrow->paintRequest(
				) | rpl::on_next([=, &icon] {
					auto p = QPainter(arrow);
					icon.paint(p, 0, 0, arrow->width());
				}, arrow->lifetime());
			}
			arrow->setCursor(style::cur_pointer);

			const auto showPicker = activeCount > 1;
			pickerButton->setVisible(showPicker);
			arrow->setVisible(showPicker);

			rpl::combine(
				title->geometryValue(),
				container->widthValue(),
				pickerButton->naturalWidthValue()
			) | rpl::on_next([=](QRect r, int width, int natural) {
				pickerButton->resizeToNaturalWidth(width / 2);
				pickerButton->moveToRight(
					st::defaultSubsectionTitlePadding.right() + arrow->width() + st::normalFont->spacew / 2,
					r.y() + (r.height() - pickerButton->height()) / 2,
					width);
				arrow->moveToLeft(
					pickerButton->x() + pickerButton->width() + st::normalFont->spacew / 2,
					r.y() + (r.height() - arrow->height()) / 2);
			}, pickerButton->lifetime());

			std::vector checkboxes{
				NestedEntry{
					tr::ayu_DontReadMessages(tr::now),
					[state] { return !AyuSettings::ghost(state->selectedUserId.current()).sendReadMessages(); },
					[state](bool v) { AyuSettings::ghost(state->selectedUserId.current()).setSendReadMessages(!v); },
					[state] { return AyuSettings::ghost(state->selectedUserId.current()).sendReadMessagesLocked(); },
					[state](bool v) { AyuSettings::ghost(state->selectedUserId.current()).setSendReadMessagesLocked(v); }
				},
				NestedEntry{
					tr::ayu_DontReadStories(tr::now),
					[state] { return !AyuSettings::ghost(state->selectedUserId.current()).sendReadStories(); },
					[state](bool v) { AyuSettings::ghost(state->selectedUserId.current()).setSendReadStories(!v); },
					[state] { return AyuSettings::ghost(state->selectedUserId.current()).sendReadStoriesLocked(); },
					[state](bool v) { AyuSettings::ghost(state->selectedUserId.current()).setSendReadStoriesLocked(v); }
				},
				NestedEntry{
					tr::ayu_DontSendOnlinePackets(tr::now),
					[state] { return !AyuSettings::ghost(state->selectedUserId.current()).sendOnlinePackets(); },
					[state](bool v) { AyuSettings::ghost(state->selectedUserId.current()).setSendOnlinePackets(!v); },
					[state] { return AyuSettings::ghost(state->selectedUserId.current()).sendOnlinePacketsLocked(); },
					[state](bool v) { AyuSettings::ghost(state->selectedUserId.current()).setSendOnlinePacketsLocked(v); }
				},
				NestedEntry{
					tr::ayu_DontSendUploadProgress(tr::now),
					[state] { return !AyuSettings::ghost(state->selectedUserId.current()).sendUploadProgress(); },
					[state](bool v) { AyuSettings::ghost(state->selectedUserId.current()).setSendUploadProgress(!v); },
					[state] { return AyuSettings::ghost(state->selectedUserId.current()).sendUploadProgressLocked(); },
					[state](bool v) { AyuSettings::ghost(state->selectedUserId.current()).setSendUploadProgressLocked(v); }
				},
				NestedEntry{
					tr::ayu_SendOfflinePacketAfterOnline(tr::now),
					[state] { return AyuSettings::ghost(state->selectedUserId.current()).sendOfflinePacketAfterOnline(); },
					[state](bool v) { AyuSettings::ghost(state->selectedUserId.current()).setSendOfflinePacketAfterOnline(v); },
					[state] { return AyuSettings::ghost(state->selectedUserId.current()).sendOfflinePacketAfterOnlineLocked(); },
					[state](bool v) { AyuSettings::ghost(state->selectedUserId.current()).setSendOfflinePacketAfterOnlineLocked(v); }
				},
			};

			auto collapsible = AddCollapsibleToggle(
				container,
				tr::ayu_GhostModeToggle(),
				std::move(checkboxes),
				true,
				tr::ayu_GhostModeOptionShiftDescription(tr::now));
			state->refreshCheckboxes = std::move(collapsible.refresh);
			if (wctx.highlights && collapsible.widget) {
				wctx.highlights->push_back(std::make_pair(
					u"ayu/ghostModeToggle"_q,
					HighlightEntry{ collapsible.widget, {} }));
			}

			const auto markReadButton = AddButtonWithIcon(
				container,
				tr::ayu_MarkReadAfterAction(),
				st::settingsButtonNoIcon
			);
			if (wctx.highlights) {
				wctx.highlights->push_back(std::make_pair(
					u"ayu/markReadAfterAction"_q,
					HighlightEntry{ markReadButton.get(), {} }));
			}
			markReadButton->toggleOn(
				state->selectedUserId.value()
				| rpl::map([](uint64 id) {
					return AyuSettings::ghost(id).markReadAfterActionValue();
				}) | rpl::flatten_latest()
			)->toggledValue(
			) | rpl::filter(
				[=](bool enabled) {
					return enabled != AyuSettings::ghost(state->selectedUserId.current()).markReadAfterAction();
				}
			) | on_next(
				[=](bool enabled) {
					auto &ghost = AyuSettings::ghost(state->selectedUserId.current());
					ghost.setMarkReadAfterAction(enabled);
					if (enabled) {
						ghost.setUseScheduledMessages(false);
					}
				},
				container->lifetime());
			AddSkip(container);
			AddDividerText(container, tr::ayu_MarkReadAfterActionDescription());

			// FurryGram: Ghost Schedule — auto on/off within a daily time window.
			AddSkip(container);
			const auto fmtMinutes = [](int m) {
				m = ((m % 1440) + 1440) % 1440;
				return QString("%1:%2")
					.arg(m / 60, 2, 10, QChar('0'))
					.arg(m % 60, 2, 10, QChar('0'));
			};
			const auto schedToggle = AddButtonWithIcon(
				container,
				rpl::single(FurryLang::Pick(u"Ghost Schedule"_q, QString::fromUtf8("\xD0\xA0\xD0\xB0\xD1\x81\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5\x20\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB7\xD1\x80\xD0\xB0\xD0\xBA\xD0\xB0"))),
				st::settingsButtonNoIcon);
			schedToggle->toggleOn(
				AyuSettings::getInstance().ghostScheduleEnabledValue()
			)->toggledValue(
			) | rpl::filter([=](bool v) {
				return v != AyuSettings::getInstance().ghostScheduleEnabled();
			}) | on_next([=](bool v) {
				AyuSettings::getInstance().setGhostScheduleEnabled(v);
			}, container->lifetime());

			// Native date-time wheel picker (like "Send this message on…"). We only
			// use the time-of-day part: seed it at today HH:MM and read back hours
			// and minutes on submit (the date is ignored — the schedule is daily).
			const auto pickTime = [=](
					const QString &title,
					Fn<int()> getter,
					Fn<void(int)> setter) {
				const auto minutes = getter();
				auto seed = QDateTime::currentDateTime();
				seed.setTime(QTime(minutes / 60, minutes % 60, 0));
				const auto seedId = base::unixtime::serialize(seed);
				controller->show(Box([=](not_null<Ui::GenericBox*> box) {
					Ui::ChooseDateTimeBox(box, {
						.title = rpl::single(title),
						.submit = rpl::single(FurryLang::Pick(
							u"Set"_q,
							QString::fromUtf8("\xD0\x93\xD0\xBE\xD1\x82\xD0\xBE\xD0\xB2\xD0\xBE"))),
						.done = [=](TimeId result) {
							const auto t = base::unixtime::parse(result).time();
							setter(t.hour() * 60 + t.minute());
							box->closeBox();
						},
						.time = seedId,
					});
				}));
			};

			const auto fromButton = AddButtonWithLabel(
				container,
				rpl::single(FurryLang::Pick(u"Ghost from"_q, QString::fromUtf8("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB7\xD1\x80\xD0\xB0\xD0\xBA\x20\xD1\x81"))),
				AyuSettings::getInstance().ghostScheduleFromValue(
				) | rpl::map(fmtMinutes),
				st::settingsButtonNoIcon);
			fromButton->setClickedCallback([=] {
				pickTime(
					FurryLang::Pick(u"Ghost from"_q, QString::fromUtf8("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB7\xD1\x80\xD0\xB0\xD0\xBA\x20\xD1\x81")),
					[] { return AyuSettings::getInstance().ghostScheduleFrom(); },
					[](int m) { AyuSettings::getInstance().setGhostScheduleFrom(m); });
			});

			const auto toButton = AddButtonWithLabel(
				container,
				rpl::single(FurryLang::Pick(u"Ghost until"_q, QString::fromUtf8("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB7\xD1\x80\xD0\xB0\xD0\xBA\x20\xD0\xB4\xD0\xBE"))),
				AyuSettings::getInstance().ghostScheduleToValue(
				) | rpl::map(fmtMinutes),
				st::settingsButtonNoIcon);
			toButton->setClickedCallback([=] {
				pickTime(
					FurryLang::Pick(u"Ghost until"_q, QString::fromUtf8("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB7\xD1\x80\xD0\xB0\xD0\xBA\x20\xD0\xB4\xD0\xBE")),
					[] { return AyuSettings::getInstance().ghostScheduleTo(); },
					[](int m) { AyuSettings::getInstance().setGhostScheduleTo(m); });
			});

			AddSkip(container);
			AddDividerText(container, rpl::single(FurryLang::Pick(
				u"Automatically enables Ghost Mode during the selected time window."_q,
				QString::fromUtf8("\xD0\x90\xD0\xB2\xD1\x82\xD0\xBE\xD0\xBC\xD0\xB0\xD1\x82\xD0\xB8\xD1\x87\xD0\xB5\xD1\x81\xD0\xBA\xD0\xB8\x20\xD0\xB2\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB0\xD0\xB5\xD1\x82\x20\xD1\x80\xD0\xB5\xD0\xB6\xD0\xB8\xD0\xBC\x20\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB7\xD1\x80\xD0\xB0\xD0\xBA\xD0\xB0\x20\xD0\xB2\x20\xD0\xB2\xD1\x8B\xD0\xB1\xD1\x80\xD0\xB0\xD0\xBD\xD0\xBD\xD0\xBE\xD0\xBC\x20\xD0\xB2\xD1\x80\xD0\xB5\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xBD\xD0\xBE\xD0\xBC\x20\xD0\xBE\xD0\xBA\xD0\xBD\xD0\xB5\x2E"))));

			AddSkip(container);
			const auto scheduleButton = AddButtonWithIcon(
				container,
				tr::ayu_UseScheduledMessages(),
				st::settingsButtonNoIcon
			);
			if (wctx.highlights) {
				wctx.highlights->push_back(std::make_pair(
					u"ayu/useScheduledMessages"_q,
					HighlightEntry{ scheduleButton.get(), {} }));
			}
			scheduleButton->toggleOn(
				state->selectedUserId.value()
				| rpl::map([](uint64 id) {
					return AyuSettings::ghost(id).useScheduledMessagesValue();
				}) | rpl::flatten_latest()
			)->toggledValue(
			) | rpl::filter(
				[=](bool enabled) {
					return enabled != AyuSettings::ghost(state->selectedUserId.current()).useScheduledMessages();
				}
			) | on_next(
				[=](bool enabled) {
					auto &ghost = AyuSettings::ghost(state->selectedUserId.current());
					ghost.setUseScheduledMessages(enabled);
					if (enabled) {
						ghost.setMarkReadAfterAction(false);
					}
				},
				container->lifetime());
			AddSkip(container);
			AddDividerText(container, tr::ayu_UseScheduledMessagesDescription());

			AddSkip(container);
			const auto silentOptions = std::vector<QString>{
				tr::ayu_SendWithoutSoundByDefaultNever(tr::now),
				tr::ayu_SendWithoutSoundByDefaultInGhostMode(tr::now),
				tr::ayu_SendWithoutSoundByDefaultAlways(tr::now),
			};
			const auto silentOptionText = state->selectedUserId.value(
			) | rpl::map([=](uint64 id) {
				return AyuSettings::ghost(id).sendWithoutSoundValue(
				) | rpl::map([=](SendWithoutSoundOption value) {
					return silentOptions[static_cast<int>(value)];
				});
			}) | rpl::flatten_latest();
			const auto silentButton = AddButtonWithLabel(
				container,
				tr::ayu_SendWithoutSoundByDefault(),
				std::move(silentOptionText),
				st::settingsButtonNoIcon);
			if (wctx.highlights) {
				wctx.highlights->push_back(std::make_pair(
					u"ayu/sendWithoutSound"_q,
					HighlightEntry{ silentButton.get(), {} }));
			}
			silentButton->addClickHandler([=] {
				controller->show(Box([=](not_null<Ui::GenericBox*> box) {
					const auto save = [=](int index) {
						AyuSettings::ghost(state->selectedUserId.current()
						).setSendWithoutSound(
							static_cast<SendWithoutSoundOption>(index));
					};
					SingleChoiceBox(box, {
						.title = tr::ayu_SendWithoutSoundByDefault(),
						.options = silentOptions,
						.initialSelection = static_cast<int>(
							AyuSettings::ghost(state->selectedUserId.current()
							).sendWithoutSound()),
						.callback = save,
					});
				}));
			});
			AddSkip(container);
			AddDividerText(container, tr::ayu_SendWithoutSoundByDefaultDescription());

			AddSkip(container);
			const auto suggestGhostModeButton = AddButtonWithIcon(
				container,
				tr::ayu_SuggestGhostModeBeforeViewingStory(),
				st::settingsButtonNoIcon);
			if (wctx.highlights) {
				wctx.highlights->push_back(std::make_pair(
					u"ayu/suggestGhostModeBeforeViewingStory"_q,
					HighlightEntry{ suggestGhostModeButton.get(), {} }));
			}
			suggestGhostModeButton->toggleOn(
				state->selectedUserId.value()
				| rpl::map([](uint64 id) {
					return AyuSettings::ghost(id).suggestGhostModeBeforeViewingStoryValue();
				}) | rpl::flatten_latest()
			)->toggledValue(
			) | rpl::filter(
				[=](bool enabled) {
					return enabled != AyuSettings::ghost(state->selectedUserId.current()).suggestGhostModeBeforeViewingStory();
				}
			) | on_next(
				[=](bool enabled) {
					AyuSettings::ghost(state->selectedUserId.current()).setSuggestGhostModeBeforeViewingStory(enabled);
				},
				container->lifetime());
			AddSkip(container);
			AddDividerText(container, tr::ayu_SuggestGhostModeBeforeViewingStoryDescription());

			auto showMenu = [=] {
				state->menu = base::make_unique_q<Ui::PopupMenu>(
					pickerButton,
					st::defaultPopupMenu);

				state->menu->addAction(
					base::make_unique_q<GlobalAction>(
						state->menu->menu(),
						st::defaultPopupMenu.menu,
						tr::ayu_GhostModeGlobalSettings(tr::now),
						state->selectedUserId.current() == 0,
						[=] { selectGhostProfile(state, 0); }));

				for (const auto &account : Core::App().domain().orderedAccounts()) {
					if (!account->sessionExists()) {
						continue;
					}
					auto user = account->session().user();
					auto id = account->session().userId().bare;
					state->menu->addAction(
						base::make_unique_q<AccountAction>(
							state->menu->menu(),
							st::defaultPopupMenu.menu,
							user,
							state->selectedUserId.current() == id,
							[=] { selectGhostProfile(state, id); }));
				}

				state->menu->popup(
					pickerButton->mapToGlobal(
						QPoint(pickerButton->width(), pickerButton->height())));
			};
			pickerButton->setClickedCallback(showMenu);
			arrow->setClickedCallback(showMenu);
		}, [&](const SearchContext &sctx) {
			sctx.entries->push_back({
				.id = u"ayu/ghostModeToggle"_q,
				.title = tr::ayu_GhostModeToggle(tr::now),
				.section = sctx.sectionId,
			});
			sctx.entries->push_back({
				.id = u"ayu/markReadAfterAction"_q,
				.title = tr::ayu_MarkReadAfterAction(tr::now),
				.section = sctx.sectionId,
			});
			sctx.entries->push_back({
				.id = u"ayu/useScheduledMessages"_q,
				.title = tr::ayu_UseScheduledMessages(tr::now),
				.section = sctx.sectionId,
			});
			sctx.entries->push_back({
				.id = u"ayu/sendWithoutSound"_q,
				.title = tr::ayu_SendWithoutSoundByDefault(tr::now),
				.section = sctx.sectionId,
			});
			sctx.entries->push_back({
				.id = u"ayu/suggestGhostModeBeforeViewingStory"_q,
				.title = tr::ayu_SuggestGhostModeBeforeViewingStory(tr::now),
				.section = sctx.sectionId,
			});
		});
	});
}

void BuildSpyEssentials(SectionBuilder &builder, AyuSectionBuilder &ayu) {
	builder.addSubsectionTitle(tr::ayu_SpyEssentialsHeader());

	ayu.addSettingToggle({
		.id = u"ayu/saveDeletedMessages"_q,
		.title = tr::ayu_SaveDeletedMessages(),
		.getter = &AyuSettings::saveDeletedMessages,
		.setter = &AyuSettings::setSaveDeletedMessages,
	});
	ayu.addSettingToggle({
		.id = u"ayu/saveMessagesHistory"_q,
		.title = tr::ayu_SaveMessagesHistory(),
		.getter = &AyuSettings::saveMessagesHistory,
		.setter = &AyuSettings::setSaveMessagesHistory,
	});

	ayu.addSectionDivider();

	ayu.addSettingToggle({
		.id = u"ayu/saveForBots"_q,
		.title = tr::ayu_MessageSavingSaveForBots(),
		.getter = &AyuSettings::saveForBots,
		.setter = &AyuSettings::setSaveForBots,
	});
}

void BuildOther(SectionBuilder &builder, AyuSectionBuilder &ayu) {
	builder.addSubsectionTitle(tr::ayu_MessageSavingOtherHeader());

	ayu.addSettingToggle({
		.id = u"ayu/localPremium"_q,
		.title = tr::ayu_LocalPremium(),
		.getter = &AyuSettings::localPremium,
		.setter = &AyuSettings::setLocalPremium,
	});
	ayu.addSettingToggle({
		.id = u"ayu/disableAds"_q,
		.title = tr::ayu_DisableAds(),
		.getter = &AyuSettings::disableAds,
		.setter = &AyuSettings::setDisableAds,
	});

	ayu.addSectionDivider();

	// FurryGram: privacy category.
	builder.addSubsectionTitle(rpl::single(FurryLang::Pick(u"Privacy"_q, QString::fromUtf8("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB0\xD1\x82\xD0\xBD\xD0\xBE\xD1\x81\xD1\x82\xD1\x8C"))));

	// FurryGram: protect the window from screenshots / screen recording / sharing.
	ayu.addSettingToggle({
		.id = u"furry/antiScreenshare"_q,
		.title = rpl::single(FurryLang::Pick(u"Anti-screenshare (hide window from capture)"_q, QString::fromUtf8("\xD0\x90\xD0\xBD\xD1\x82\xD0\xB8\x2D\xD1\x81\xD0\xBA\xD1\x80\xD0\xB8\xD0\xBD\xD1\x88\xD0\xB5\xD1\x80\xD0\xB8\xD0\xBD\xD0\xB3\x20\x28\xD1\x81\xD0\xBA\xD1\x80\xD1\x8B\xD1\x82\xD1\x8C\x20\xD0\xBE\xD0\xBA\xD0\xBD\xD0\xBE\x20\xD0\xB8\xD0\xB7\x20\xD0\xB7\xD0\xB0\xD1\x85\xD0\xB2\xD0\xB0\xD1\x82\xD0\xB0\x29"))),
		.getter = &AyuSettings::antiScreenshare,
		.setter = &AyuSettings::setAntiScreenshare,
	});

	ayu.addSectionDivider();

	// FurryGram: interface category.
	builder.addSubsectionTitle(rpl::single(FurryLang::Pick(
		u"Interface"_q,
		// "Интерфейс"
		QString::fromUtf8("\xD0\x98\xD0\xBD\xD1\x82\xD0\xB5\xD1\x80\xD1\x84\xD0\xB5\xD0\xB9\xD1\x81"))));

	// FurryGram: rounded "pill" compose bar floating over the wallpaper, with
	// the send button highlighted by an accent ring. Stock bar when off.
	ayu.addSettingToggle({
		.id = u"furry/composeBar"_q,
		.title = rpl::single(FurryLang::Pick(
			u"Rounded compose bar"_q,
			// "Скруглённое поле ввода"
			QString::fromUtf8("\xD0\xA1\xD0\xBA\xD1\x80\xD1\x83\xD0\xB3\xD0\xBB\xD1\x91\xD0\xBD\xD0\xBD\xD0\xBE\xD0\xB5\x20\xD0\xBF\xD0\xBE\xD0\xBB\xD0\xB5\x20\xD0\xB2\xD0\xB2\xD0\xBE\xD0\xB4\xD0\xB0"))),
		.getter = &AyuSettings::furryComposeBar,
		.setter = &AyuSettings::setFurryComposeBar,
	});

	ayu.addSectionDivider();

	// FurryGram: notifications category.
	builder.addSubsectionTitle(rpl::single(FurryLang::Pick(
		u"Notifications"_q,
		// "Уведомления"
		QString::fromUtf8("\xD0\xA3\xD0\xB2\xD0\xB5\xD0\xB4\xD0\xBE\xD0\xBC\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F"))));

	// FurryGram: Focus mode master toggle. Profile editing UI comes next; this
	// turns Focus on/off using the active profile's rules.
	ayu.addSettingToggle({
		.id = u"furry/focusEnabled"_q,
		.title = rpl::single(FurryLang::Pick(
			u"Focus mode"_q,
			// "Режим фокуса"
			QString::fromUtf8("\xD0\xA0\xD0\xB5\xD0\xB6\xD0\xB8\xD0\xBC\x20\xD1\x84\xD0\xBE\xD0\xBA\xD1\x83\xD1\x81\xD0\xB0"))),
		.getter = &AyuSettings::focusEnabled,
		.setter = &AyuSettings::setFocusEnabled,
	});

	// FurryGram: Focus profile editor. Edits the ACTIVE profile; the "Active
	// profile" row cycles through them. Reactivity rides focusSettingsChanges
	// (fired on profile switch and on any rule edit). Add/rename/delete and the
	// folder allow-list picker are a later sub-phase.
	builder.add([](const BuildContext &ctx) {
		v::match(ctx, [&](const WidgetContext &wctx) {
			const auto container = wctx.container;
			const auto controller = wctx.controller;

			const auto onFocus = [] {
				return rpl::single(rpl::empty) | rpl::then(
					AyuSettings::getInstance().focusSettingsChanges());
			};
			const auto activeName = [] {
				const auto &ps = AyuSettings::getInstance().focusProfiles();
				const auto i = AyuSettings::getInstance().focusActiveProfile();
				return (i >= 0 && i < int(ps.size()))
					? QString::fromStdString(ps[i].name)
					: QString("Focus");
			};

			const auto activeButton = AddButtonWithLabel(
				container,
				rpl::single(FurryLang::Pick(u"Active profile"_q, QString::fromUtf8("\xD0\x90\xD0\xBA\xD1\x82\xD0\xB8\xD0\xB2\xD0\xBD\xD1\x8B\xD0\xB9\x20\xD0\xBF\xD1\x80\xD0\xBE\xD1\x84\xD0\xB8\xD0\xBB\xD1\x8C"))),
				onFocus() | rpl::map([=] { return activeName(); }),
				st::settingsButtonNoIcon);
			activeButton->setClickedCallback([=] {
				const auto count = int(
					AyuSettings::getInstance().focusProfiles().size());
				if (count > 0) {
					AyuSettings::getInstance().setFocusActiveProfile(
						(AyuSettings::getInstance().focusActiveProfile() + 1)
							% count);
				}
			});

			// Add / rename / delete profiles.
			const auto addProfileButton = AddButtonWithIcon(
				container,
				rpl::single(FurryLang::Pick(u"Add profile"_q, QString::fromUtf8("\xD0\x94\xD0\xBE\xD0\xB1\xD0\xB0\xD0\xB2\xD0\xB8\xD1\x82\xD1\x8C\x20\xD0\xBF\xD1\x80\xD0\xBE\xD1\x84\xD0\xB8\xD0\xBB\xD1\x8C"))),
				st::settingsButtonNoIcon);
			addProfileButton->setClickedCallback([=] {
				auto &s = AyuSettings::getInstance();
				auto profiles = s.focusProfiles();
				auto profile = FocusProfile();
				profile.name = "Focus "
					+ std::to_string(profiles.size() + 1);
				profiles.push_back(profile);
				s.setFocusProfiles(profiles);
				s.setFocusActiveProfile(int(profiles.size()) - 1);
			});

			const auto renameProfileButton = AddButtonWithIcon(
				container,
				rpl::single(FurryLang::Pick(u"Rename profile"_q, QString::fromUtf8("\xD0\x9F\xD0\xB5\xD1\x80\xD0\xB5\xD0\xB8\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xB0\xD1\x82\xD1\x8C\x20\xD0\xBF\xD1\x80\xD0\xBE\xD1\x84\xD0\xB8\xD0\xBB\xD1\x8C"))),
				st::settingsButtonNoIcon);
			renameProfileButton->setClickedCallback([=] {
				const auto current = QString::fromStdString(
					AyuSettings::getInstance().activeFocusProfile().name);
				controller->show(Box([=](not_null<Ui::GenericBox*> box) {
					box->setTitle(rpl::single(FurryLang::Pick(u"Rename profile"_q, QString::fromUtf8("\xD0\x9F\xD0\xB5\xD1\x80\xD0\xB5\xD0\xB8\xD0\xBC\xD0\xB5\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xB0\xD1\x82\xD1\x8C\x20\xD0\xBF\xD1\x80\xD0\xBE\xD1\x84\xD0\xB8\xD0\xBB\xD1\x8C"))));
					const auto input = box->addRow(object_ptr<Ui::InputField>(
						box,
						st::defaultInputField,
						rpl::single(FurryLang::Pick(u"Name"_q, QString::fromUtf8("\xD0\x98\xD0\xBC\xD1\x8F"))),
						current));
					box->setFocusCallback([=] { input->setFocusFast(); });
					const auto submit = [=] {
						const auto name = input->getLastText().trimmed();
						if (!name.isEmpty()) {
							AyuSettings::getInstance().modifyActiveFocusProfile(
								[=](FocusProfile &p) {
									p.name = name.toStdString();
								});
						}
						box->closeBox();
					};
					input->submits(
					) | rpl::on_next([=](auto) { submit(); }, box->lifetime());
					box->addButton(rpl::single(FurryLang::Pick(u"Save"_q, QString::fromUtf8("\xD0\xA1\xD0\xBE\xD1\x85\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB8\xD1\x82\xD1\x8C"))), submit);
					box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
				}));
			});

			const auto deleteProfileButton = AddButtonWithIcon(
				container,
				rpl::single(FurryLang::Pick(u"Delete profile"_q, QString::fromUtf8("\xD0\xA3\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xB8\xD1\x82\xD1\x8C\x20\xD0\xBF\xD1\x80\xD0\xBE\xD1\x84\xD0\xB8\xD0\xBB\xD1\x8C"))),
				st::settingsButtonNoIcon);
			deleteProfileButton->setClickedCallback([=] {
				auto &s = AyuSettings::getInstance();
				auto profiles = s.focusProfiles();
				const auto idx = s.focusActiveProfile();
				if (int(profiles.size()) <= 1
					|| idx < 0
					|| idx >= int(profiles.size())) {
					return;
				}
				profiles.erase(profiles.begin() + idx);
				s.setFocusProfiles(profiles);
				s.setFocusActiveProfile(
					std::min(idx, int(profiles.size()) - 1));
			});

			const auto addRule = [&](
					const QString &text,
					Fn<bool(const FocusProfile&)> get,
					Fn<void(FocusProfile&, bool)> set) {
				const auto btn = AddButtonWithIcon(
					container,
					rpl::single(text),
					st::settingsButtonNoIcon);
				btn->toggleOn(
					onFocus() | rpl::map([=] {
						return get(
							AyuSettings::getInstance().activeFocusProfile());
					})
				)->toggledValue(
				) | rpl::filter([=](bool v) {
					return v != get(
						AyuSettings::getInstance().activeFocusProfile());
				}) | on_next([=](bool v) {
					AyuSettings::getInstance().modifyActiveFocusProfile(
						[=](FocusProfile &p) { set(p, v); });
				}, container->lifetime());
			};

			addRule(
				FurryLang::Pick(u"Hide popups (otherwise show them silently)"_q, QString::fromUtf8("\xD0\xA1\xD0\xBA\xD1\x80\xD1\x8B\xD0\xB2\xD0\xB0\xD1\x82\xD1\x8C\x20\xD0\xBF\xD0\xBE\xD0\xBF\xD0\xB0\xD0\xBF\xD1\x8B\x20\x28\xD0\xB8\xD0\xBD\xD0\xB0\xD1\x87\xD0\xB5\x20\xD0\xBF\xD0\xBE\xD0\xBA\xD0\xB0\xD0\xB7\xD1\x8B\xD0\xB2\xD0\xB0\xD1\x82\xD1\x8C\x20\xD0\xB1\xD0\xB5\xD0\xB7\x20\xD0\xB7\xD0\xB2\xD1\x83\xD0\xBA\xD0\xB0\x29")),
				[](const FocusProfile &p) { return p.hideToast; },
				[](FocusProfile &p, bool v) { p.hideToast = v; });
			addRule(
				FurryLang::Pick(u"Allow pinned chats"_q, QString::fromUtf8("\xD0\x9F\xD1\x80\xD0\xBE\xD0\xBF\xD1\x83\xD1\x81\xD0\xBA\xD0\xB0\xD1\x82\xD1\x8C\x20\xD0\xB7\xD0\xB0\xD0\xBA\xD1\x80\xD0\xB5\xD0\xBF\xD0\xBB\xD1\x91\xD0\xBD\xD0\xBD\xD1\x8B\xD0\xB5\x20\xD1\x87\xD0\xB0\xD1\x82\xD1\x8B")),
				[](const FocusProfile &p) { return p.allowPinned; },
				[](FocusProfile &p, bool v) { p.allowPinned = v; });
			addRule(
				FurryLang::Pick(u"Allow private chats"_q, QString::fromUtf8("\xD0\x9F\xD1\x80\xD0\xBE\xD0\xBF\xD1\x83\xD1\x81\xD0\xBA\xD0\xB0\xD1\x82\xD1\x8C\x20\xD0\xBB\xD0\xB8\xD1\x87\xD0\xBD\xD1\x8B\xD0\xB5\x20\xD1\x87\xD0\xB0\xD1\x82\xD1\x8B")),
				[](const FocusProfile &p) { return p.allowPrivateOnly; },
				[](FocusProfile &p, bool v) { p.allowPrivateOnly = v; });
			addRule(
				FurryLang::Pick(u"Honour per-chat exceptions"_q, QString::fromUtf8("\xD0\xA3\xD1\x87\xD0\xB8\xD1\x82\xD1\x8B\xD0\xB2\xD0\xB0\xD1\x82\xD1\x8C\x20\xD0\xB8\xD1\x81\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F\x20\xD1\x87\xD0\xB0\xD1\x82\xD0\xBE\xD0\xB2")),
				[](const FocusProfile &p) { return p.allowExceptions; },
				[](FocusProfile &p, bool v) { p.allowExceptions = v; });

			// Allow a whole folder's chats through Focus.
			const auto folderButton = AddButtonWithLabel(
				container,
				rpl::single(FurryLang::Pick(u"Allow a folder"_q, QString::fromUtf8("\xD0\x9F\xD1\x80\xD0\xBE\xD0\xBF\xD1\x83\xD1\x81\xD0\xBA\xD0\xB0\xD1\x82\xD1\x8C\x20\xD0\xBF\xD0\xB0\xD0\xBF\xD0\xBA\xD1\x83"))),
				onFocus() | rpl::map([=] {
					const auto id = AyuSettings::getInstance()
						.activeFocusProfile().allowFolderId;
					if (!id) {
						return FurryLang::Pick(u"None"_q, QString::fromUtf8("\xD0\x9D\xD0\xB5\xD1\x82"));
					}
					for (const auto &filter : controller->session()
							.data().chatsFilters().list()) {
						if (filter.id() == FilterId(id)) {
							return filter.titleText().text;
						}
					}
					return FurryLang::Pick(u"None"_q, QString::fromUtf8("\xD0\x9D\xD0\xB5\xD1\x82"));
				}),
				st::settingsButtonNoIcon);
			folderButton->setClickedCallback([=] {
				controller->show(Box([=](not_null<Ui::GenericBox*> box) {
					box->setTitle(rpl::single(FurryLang::Pick(u"Allow a folder"_q, QString::fromUtf8("\xD0\x9F\xD1\x80\xD0\xBE\xD0\xBF\xD1\x83\xD1\x81\xD0\xBA\xD0\xB0\xD1\x82\xD1\x8C\x20\xD0\xBF\xD0\xB0\xD0\xBF\xD0\xBA\xD1\x83"))));
					const auto inner = box->verticalLayout();
					const auto choose = [=](int folderId) {
						AyuSettings::getInstance().modifyActiveFocusProfile(
							[=](FocusProfile &p) {
								p.allowFolderId = folderId;
							});
						box->closeBox();
					};
					const auto noneRow = AddButtonWithIcon(
						inner,
						rpl::single(FurryLang::Pick(u"None"_q, QString::fromUtf8("\xD0\x9D\xD0\xB5\xD1\x82"))),
						st::settingsButtonNoIcon);
					noneRow->setClickedCallback([=] { choose(0); });
					for (const auto &filter : controller->session()
							.data().chatsFilters().list()) {
						const auto id = int(filter.id());
						if (!id) {
							continue;
						}
						const auto row = AddButtonWithIcon(
							inner,
							rpl::single(filter.titleText().text),
							st::settingsButtonNoIcon);
						row->setClickedCallback([=] { choose(id); });
					}
					box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
				}));
			});

			const auto fmtMinutes = [](int m) {
				m = ((m % 1440) + 1440) % 1440;
				return QString("%1:%2")
					.arg(m / 60, 2, 10, QChar('0'))
					.arg(m % 60, 2, 10, QChar('0'));
			};
			const auto schedToggle = AddButtonWithIcon(
				container,
				rpl::single(FurryLang::Pick(u"Schedule"_q, QString::fromUtf8("\xD0\xA0\xD0\xB0\xD1\x81\xD0\xBF\xD0\xB8\xD1\x81\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5"))),
				st::settingsButtonNoIcon);
			schedToggle->toggleOn(
				onFocus() | rpl::map([=] {
					return AyuSettings::getInstance()
						.activeFocusProfile().scheduleEnabled;
				})
			)->toggledValue(
			) | rpl::filter([=](bool v) {
				return v != AyuSettings::getInstance()
					.activeFocusProfile().scheduleEnabled;
			}) | on_next([=](bool v) {
				AyuSettings::getInstance().modifyActiveFocusProfile(
					[=](FocusProfile &p) { p.scheduleEnabled = v; });
			}, container->lifetime());

			const auto pickTime = [=](
					const QString &title,
					Fn<int()> getter,
					Fn<void(int)> setter) {
				const auto minutes = getter();
				auto seed = QDateTime::currentDateTime();
				seed.setTime(QTime(minutes / 60, minutes % 60, 0));
				const auto seedId = base::unixtime::serialize(seed);
				controller->show(Box([=](not_null<Ui::GenericBox*> box) {
					Ui::ChooseDateTimeBox(box, {
						.title = rpl::single(title),
						.submit = rpl::single(FurryLang::Pick(
							u"Set"_q,
							QString::fromUtf8("\xD0\x93\xD0\xBE\xD1\x82\xD0\xBE\xD0\xB2\xD0\xBE"))),
						.done = [=](TimeId result) {
							const auto t = base::unixtime::parse(result).time();
							setter(t.hour() * 60 + t.minute());
							box->closeBox();
						},
						.time = seedId,
					});
				}));
			};

			const auto fromButton = AddButtonWithLabel(
				container,
				rpl::single(FurryLang::Pick(u"From"_q, QString::fromUtf8("\xD0\xA1"))),
				onFocus() | rpl::map([=] {
					return fmtMinutes(AyuSettings::getInstance()
						.activeFocusProfile().scheduleFrom);
				}),
				st::settingsButtonNoIcon);
			fromButton->setClickedCallback([=] {
				pickTime(
					FurryLang::Pick(u"From"_q, QString::fromUtf8("\xD0\xA1")),
					[] {
						return AyuSettings::getInstance()
							.activeFocusProfile().scheduleFrom;
					},
					[](int m) {
						AyuSettings::getInstance().modifyActiveFocusProfile(
							[=](FocusProfile &p) { p.scheduleFrom = m; });
					});
			});

			const auto toButton = AddButtonWithLabel(
				container,
				rpl::single(FurryLang::Pick(u"Until"_q, QString::fromUtf8("\xD0\x94\xD0\xBE"))),
				onFocus() | rpl::map([=] {
					return fmtMinutes(AyuSettings::getInstance()
						.activeFocusProfile().scheduleTo);
				}),
				st::settingsButtonNoIcon);
			toButton->setClickedCallback([=] {
				pickTime(
					FurryLang::Pick(u"Until"_q, QString::fromUtf8("\xD0\x94\xD0\xBE")),
					[] {
						return AyuSettings::getInstance()
							.activeFocusProfile().scheduleTo;
					},
					[](int m) {
						AyuSettings::getInstance().modifyActiveFocusProfile(
							[=](FocusProfile &p) { p.scheduleTo = m; });
					});
			});

			const auto exceptionsButton = AddButtonWithIcon(
				container,
				rpl::single(FurryLang::Pick(u"Always notify in Focus"_q, QString::fromUtf8("\xD0\x92\xD1\x81\xD0\xB5\xD0\xB3\xD0\xB4\xD0\xB0\x20\xD1\x83\xD0\xB2\xD0\xB5\xD0\xB4\xD0\xBE\xD0\xBC\xD0\xBB\xD1\x8F\xD1\x82\xD1\x8C\x20\xD0\xB2\x20\xD1\x84\xD0\xBE\xD0\xBA\xD1\x83\xD1\x81\xD0\xB5"))),
				st::settingsButtonNoIcon);
			exceptionsButton->setClickedCallback([=] {
				Ayu::Focus::ShowExceptionsBox(controller);
			});

			AddSkip(container);
			AddDividerText(container, rpl::single(FurryLang::Pick(
				u"While Focus is on, only allowed chats notify; the rest are "
				"silenced or hidden by the active profile."_q,
				QString::fromUtf8("\xD0\x9A\xD0\xBE\xD0\xB3\xD0\xB4\xD0\xB0\x20\xD1\x84\xD0\xBE\xD0\xBA\xD1\x83\xD1\x81\x20\xD0\xB2\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD1\x91\xD0\xBD\x2C\x20\xD1\x83\xD0\xB2\xD0\xB5\xD0\xB4\xD0\xBE\xD0\xBC\xD0\xBB\xD1\x8F\xD1\x8E\xD1\x82\x20\xD1\x82\xD0\xBE\xD0\xBB\xD1\x8C\xD0\xBA\xD0\xBE\x20\xD1\x80\xD0\xB0\xD0\xB7\xD1\x80\xD0\xB5\xD1\x88\xD1\x91\xD0\xBD\xD0\xBD\xD1\x8B\xD0\xB5\x20\xD1\x87\xD0\xB0\xD1\x82\xD1\x8B\x3B\x20\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0\xD0\xBB\xD1\x8C\xD0\xBD\xD1\x8B\xD0\xB5\x20\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB3\xD0\xBB\xD1\x83\xD1\x88\xD0\xB0\xD1\x8E\xD1\x82\xD1\x81\xD1\x8F\x20\xD0\xB8\xD0\xBB\xD0\xB8\x20\xD1\x81\xD0\xBA\xD1\x80\xD1\x8B\xD0\xB2\xD0\xB0\xD1\x8E\xD1\x82\xD1\x81\xD1\x8F\x20\xD0\xBF\xD0\xBE\x20\xD0\xB0\xD0\xBA\xD1\x82\xD0\xB8\xD0\xB2\xD0\xBD\xD0\xBE\xD0\xBC\xD1\x83\x20\xD0\xBF\xD1\x80\xD0\xBE\xD1\x84\xD0\xB8\xD0\xBB\xD1\x8E\x2E"))));
		}, [](const SearchContext &) {
			// Focus profile rows are not search-indexed yet.
		});
	});

	ayu.addSectionDivider();

	// FurryGram: on-device (offline) recognition category — Whisper voice
	// transcription and Tesseract OCR.
	builder.addSubsectionTitle(rpl::single(FurryLang::Pick(
		u"On-device recognition"_q,
		// "Распознавание на устройстве"
		QString::fromUtf8("\xD0\xA0\xD0\xB0\xD1\x81\xD0\xBF\xD0\xBE\xD0\xB7\xD0\xBD\xD0\xB0\xD0\xB2\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5\x20\xD0\xBD\xD0\xB0\x20\xD1\x83\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xB9\xD1\x81\xD1\x82\xD0\xB2\xD0\xB5"))));

	// FurryGram: offline voice-message transcription via local Whisper model.
	// A single expandable entry that reveals all of its detailed settings:
	// the enable toggle, the model-size picker and the model downloader.
	const auto whisperExpanded = std::make_shared<rpl::variable<bool>>(false);
	builder.addButton({
		.id = u"furry/voiceTranscription"_q,
		.title = rpl::single(FurryLang::Pick(
			u"Local voice transcription (Whisper)"_q,
			// "Локальная расшифровка голоса (Whisper)"
			QString::fromUtf8("\xD0\x9B\xD0\xBE\xD0\xBA\xD0\xB0\xD0\xBB\xD1\x8C\xD0\xBD\xD0\xB0\xD1\x8F\x20\xD1\x80\xD0\xB0\xD1\x81\xD1\x88\xD0\xB8\xD1\x84\xD1\x80\xD0\xBE\xD0\xB2\xD0\xBA\xD0\xB0\x20\xD0\xB3\xD0\xBE\xD0\xBB\xD0\xBE\xD1\x81\xD0\xB0\x20\x28\x57\x68\x69\x73\x70\x65\x72\x29"))),
		.st = &st::settingsButtonNoIcon,
		.label = AyuSettings::getInstance().localTranscribeValue(
		) | rpl::map([](bool on) {
			return on
				? FurryLang::Pick(
					u"On"_q,
					// "Вкл"
					QString::fromUtf8("\xD0\x92\xD0\xBA\xD0\xBB"))
				: FurryLang::Pick(
					u"Off"_q,
					// "Выкл"
					QString::fromUtf8("\xD0\x92\xD1\x8B\xD0\xBA\xD0\xBB"));
		}),
		.onClick = [=] {
			*whisperExpanded = !whisperExpanded->current();
		},
	});

	builder.scope([&] {
		// The feature toggle itself (frees the model from RAM when turned off).
		ayu.addToggle({
			.id = u"furry/localTranscribe"_q,
			.title = rpl::single(FurryLang::Pick(
				u"Enable"_q,
				// "Включить"
				QString::fromUtf8("\xD0\x92\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB8\xD1\x82\xD1\x8C"))),
			.getter = [] {
				return AyuSettings::getInstance().localTranscribe();
			},
			.setter = [](bool v) {
				AyuSettings::getInstance().setLocalTranscribe(v);
				if (!v) {
					Ayu::Voice::FreeContext();
				}
			},
		});

		// Spoken language (an explicit language is more accurate than auto).
		const auto langCodes = std::vector<QString>{
			u"auto"_q, u"en"_q, u"ru"_q, u"uk"_q, u"es"_q, u"de"_q,
			u"fr"_q, u"it"_q, u"pt"_q, u"pl"_q, u"ja"_q, u"zh"_q };
		const auto currentLang = AyuSettings::getInstance().whisperLanguage();
		auto langIndex = 0;
		for (auto i = 0; i != int(langCodes.size()); ++i) {
			if (langCodes[i] == currentLang) {
				langIndex = i;
				break;
			}
		}
		ayu.addChooseButton({
			.id = u"furry/whisperLanguage"_q,
			.title = rpl::single(FurryLang::Pick(
				u"Language"_q,
				// "Язык"
				QString::fromUtf8("\xD0\xAF\xD0\xB7\xD1\x8B\xD0\xBA"))),
			.boxTitle = rpl::single(FurryLang::Pick(
				u"Spoken language"_q,
				// "Язык речи"
				QString::fromUtf8("\xD0\xAF\xD0\xB7\xD1\x8B\xD0\xBA\x20\xD1\x80\xD0\xB5\xD1\x87\xD0\xB8"))),
			.initialSelection = langIndex,
			.options = {
				// "Автоопределение"
				FurryLang::Pick(u"Auto-detect"_q, QString::fromUtf8("\xD0\x90\xD0\xB2\xD1\x82\xD0\xBE\xD0\xBE\xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD0\xB5\xD0\xBB\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5")),
				// "Английский"
				FurryLang::Pick(u"English"_q, QString::fromUtf8("\xD0\x90\xD0\xBD\xD0\xB3\xD0\xBB\xD0\xB8\xD0\xB9\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9")),
				// "Русский"
				FurryLang::Pick(u"Russian"_q, QString::fromUtf8("\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9")),
				// "Украинский"
				FurryLang::Pick(u"Ukrainian"_q, QString::fromUtf8("\xD0\xA3\xD0\xBA\xD1\x80\xD0\xB0\xD0\xB8\xD0\xBD\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9")),
				// "Испанский"
				FurryLang::Pick(u"Spanish"_q, QString::fromUtf8("\xD0\x98\xD1\x81\xD0\xBF\xD0\xB0\xD0\xBD\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9")),
				// "Немецкий"
				FurryLang::Pick(u"German"_q, QString::fromUtf8("\xD0\x9D\xD0\xB5\xD0\xBC\xD0\xB5\xD1\x86\xD0\xBA\xD0\xB8\xD0\xB9")),
				// "Французский"
				FurryLang::Pick(u"French"_q, QString::fromUtf8("\xD0\xA4\xD1\x80\xD0\xB0\xD0\xBD\xD1\x86\xD1\x83\xD0\xB7\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9")),
				// "Итальянский"
				FurryLang::Pick(u"Italian"_q, QString::fromUtf8("\xD0\x98\xD1\x82\xD0\xB0\xD0\xBB\xD1\x8C\xD1\x8F\xD0\xBD\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9")),
				// "Португальский"
				FurryLang::Pick(u"Portuguese"_q, QString::fromUtf8("\xD0\x9F\xD0\xBE\xD1\x80\xD1\x82\xD1\x83\xD0\xB3\xD0\xB0\xD0\xBB\xD1\x8C\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9")),
				// "Польский"
				FurryLang::Pick(u"Polish"_q, QString::fromUtf8("\xD0\x9F\xD0\xBE\xD0\xBB\xD1\x8C\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9")),
				// "Японский"
				FurryLang::Pick(u"Japanese"_q, QString::fromUtf8("\xD0\xAF\xD0\xBF\xD0\xBE\xD0\xBD\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9")),
				// "Китайский"
				FurryLang::Pick(u"Chinese"_q, QString::fromUtf8("\xD0\x9A\xD0\xB8\xD1\x82\xD0\xB0\xD0\xB9\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9")),
			},
			.setter = [langCodes](int i) {
				const auto code = (i >= 0 && i < int(langCodes.size()))
					? langCodes[i]
					: u"auto"_q;
				AyuSettings::getInstance().setWhisperLanguage(code);
			},
		});

		// Translate the recognized speech to English.
		ayu.addSettingToggle({
			.id = u"furry/whisperTranslate"_q,
			.title = rpl::single(FurryLang::Pick(
				u"Translate to English"_q,
				// "Переводить на английский"
				QString::fromUtf8("\xD0\x9F\xD0\xB5\xD1\x80\xD0\xB5\xD0\xB2\xD0\xBE\xD0\xB4\xD0\xB8\xD1\x82\xD1\x8C\x20\xD0\xBD\xD0\xB0\x20\xD0\xB0\xD0\xBD\xD0\xB3\xD0\xBB\xD0\xB8\xD0\xB9\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9"))),
			.getter = &AyuSettings::whisperTranslate,
			.setter = &AyuSettings::setWhisperTranslate,
		});

		// Model size picker.
		const auto currentSize = AyuSettings::getInstance().whisperModel();
		const auto sizeIndex = (currentSize == u"small"_q)
			? 1
			: (currentSize == u"medium"_q)
			? 2
			: 0;
		ayu.addChooseButton({
			.id = u"furry/whisperModel"_q,
			.title = rpl::single(FurryLang::Pick(
				u"Model size"_q,
				// "Размер модели"
				QString::fromUtf8("\xD0\xA0\xD0\xB0\xD0\xB7\xD0\xBC\xD0\xB5\xD1\x80\x20\xD0\xBC\xD0\xBE\xD0\xB4\xD0\xB5\xD0\xBB\xD0\xB8"))),
			.boxTitle = rpl::single(FurryLang::Pick(
				u"Whisper model size"_q,
				// "Размер модели Whisper"
				QString::fromUtf8("\xD0\xA0\xD0\xB0\xD0\xB7\xD0\xBC\xD0\xB5\xD1\x80\x20\xD0\xBC\xD0\xBE\xD0\xB4\xD0\xB5\xD0\xBB\xD0\xB8\x20\x57\x68\x69\x73\x70\x65\x72"))),
			.initialSelection = sizeIndex,
			.options = {
				// "base (~148 МБ, быстрее всех)"
				FurryLang::Pick(u"base (~148 MB, fastest)"_q, QString::fromUtf8("\x62\x61\x73\x65\x20\x28\x7E\x31\x34\x38\x20\xD0\x9C\xD0\x91\x2C\x20\xD0\xB1\xD1\x8B\xD1\x81\xD1\x82\xD1\x80\xD0\xB5\xD0\xB5\x20\xD0\xB2\xD1\x81\xD0\xB5\xD1\x85\x29")),
				// "small (~488 МБ)"
				FurryLang::Pick(u"small (~488 MB)"_q, QString::fromUtf8("\x73\x6D\x61\x6C\x6C\x20\x28\x7E\x34\x38\x38\x20\xD0\x9C\xD0\x91\x29")),
				// "medium (~1.5 ГБ, лучшее качество)"
				FurryLang::Pick(u"medium (~1.5 GB, best)"_q, QString::fromUtf8("\x6D\x65\x64\x69\x75\x6D\x20\x28\x7E\x31\x2E\x35\x20\xD0\x93\xD0\x91\x2C\x20\xD0\xBB\xD1\x83\xD1\x87\xD1\x88\xD0\xB5\xD0\xB5\x20\xD0\xBA\xD0\xB0\xD1\x87\xD0\xB5\xD1\x81\xD1\x82\xD0\xB2\xD0\xBE\x29")),
			},
			.setter = [](int i) {
				const auto id = (i == 1)
					? u"small"_q
					: (i == 2)
					? u"medium"_q
					: u"base"_q;
				AyuSettings::getInstance().setWhisperModel(id);
				Ayu::Voice::FreeContext();
				Ayu::Voice::RefreshModelStatus();
			},
		});

		// Download / cancel / delete the selected model.
		builder.addButton({
			.id = u"furry/whisperModelDownload"_q,
			.title = Ayu::Voice::ModelStatusValue(
			) | rpl::map([](Ayu::Voice::ModelProgress p) {
				using S = Ayu::Voice::ModelStatus;
				switch (p.status) {
				case S::Ready:
					// "Модель загружена, нажмите чтобы удалить"
					return FurryLang::Pick(
						u"Model downloaded - tap to delete"_q,
						QString::fromUtf8("\xD0\x9C\xD0\xBE\xD0\xB4\xD0\xB5\xD0\xBB\xD1\x8C\x20\xD0\xB7\xD0\xB0\xD0\xB3\xD1\x80\xD1\x83\xD0\xB6\xD0\xB5\xD0\xBD\xD0\xB0\x2C\x20\xD0\xBD\xD0\xB0\xD0\xB6\xD0\xBC\xD0\xB8\xD1\x82\xD0\xB5\x20\xD1\x87\xD1\x82\xD0\xBE\xD0\xB1\xD1\x8B\x20\xD1\x83\xD0\xB4\xD0\xB0\xD0\xBB\xD0\xB8\xD1\x82\xD1\x8C"));
				case S::Downloading:
					// "Загрузка модели... %1%"
					return FurryLang::Pick(
						u"Downloading model... %1%"_q,
						QString::fromUtf8("\xD0\x97\xD0\xB0\xD0\xB3\xD1\x80\xD1\x83\xD0\xB7\xD0\xBA\xD0\xB0\x20\xD0\xBC\xD0\xBE\xD0\xB4\xD0\xB5\xD0\xBB\xD0\xB8\x2E\x2E\x2E\x20\x25\x31\x25")).arg(p.percent);
				default:
					// "Загрузить модель"
					return FurryLang::Pick(
						u"Download model"_q,
						QString::fromUtf8("\xD0\x97\xD0\xB0\xD0\xB3\xD1\x80\xD1\x83\xD0\xB7\xD0\xB8\xD1\x82\xD1\x8C\x20\xD0\xBC\xD0\xBE\xD0\xB4\xD0\xB5\xD0\xBB\xD1\x8C"));
				}
			}),
			.st = &st::settingsButtonNoIcon,
			.onClick = [] {
				using S = Ayu::Voice::ModelStatus;
				switch (Ayu::Voice::CurrentModelStatus()) {
				case S::Downloading:
					Ayu::Voice::CancelModelDownload();
					break;
				case S::Ready:
					Ayu::Voice::DeleteModel();
					break;
				default:
					Ayu::Voice::StartModelDownload();
					break;
				}
			},
		});
	}, whisperExpanded->value() | rpl::map([whisperExpanded](bool v) {
		return v;
	}));

	// FurryGram: local OCR (Tesseract) — per-language traineddata downloads.
	// Recognition uses every language present here; right-click a photo in the
	// viewer -> "Recognize text (OCR)".
	const auto ocrExpanded = std::make_shared<rpl::variable<bool>>(false);
	builder.addButton({
		.id = u"furry/ocr"_q,
		.title = rpl::single(FurryLang::Pick(u"Text recognition (OCR)"_q, QString::fromUtf8("\xD0\xA0\xD0\xB0\xD1\x81\xD0\xBF\xD0\xBE\xD0\xB7\xD0\xBD\xD0\xB0\xD0\xB2\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5\x20\xD1\x82\xD0\xB5\xD0\xBA\xD1\x81\xD1\x82\xD0\xB0\x20\x28\x4F\x43\x52\x29"))),
		.st = &st::settingsButtonNoIcon,
		.onClick = [=] {
			*ocrExpanded = !ocrExpanded->current();
		},
	});
	builder.scope([&] {
		struct OcrLang {
			QString code;
			QString name;
		};
		const auto langs = std::vector<OcrLang>{
			{ u"eng"_q, QString("English") },
			{ u"rus"_q, QString("Russian") },
			{ u"ukr"_q, QString("Ukrainian") },
			{ u"deu"_q, QString("German") },
			{ u"fra"_q, QString("French") },
			{ u"spa"_q, QString("Spanish") },
			{ u"ita"_q, QString("Italian") },
			{ u"por"_q, QString("Portuguese") },
		};
		for (const auto &lang : langs) {
			const auto code = lang.code;
			const auto name = lang.name;
			builder.addButton({
				.id = u"furry/ocrLang/"_q + code,
				.title = Ayu::Ocr::LangStatusValue(code
				) | rpl::map([=](Ayu::Ocr::LangProgress p) {
					using S = Ayu::Ocr::LangStatus;
					switch (p.status) {
					case S::Ready:
						return name + QString(" - downloaded (tap to delete)");
					case S::Downloading:
						return name
							+ QString(" - downloading... %1%").arg(p.percent);
					default:
						return name + QString(" - download (~15 MB)");
					}
				}),
				.st = &st::settingsButtonNoIcon,
				.onClick = [=] {
					using S = Ayu::Ocr::LangStatus;
					switch (Ayu::Ocr::CurrentLangStatus(code)) {
					case S::Downloading:
						Ayu::Ocr::CancelLangDownload(code);
						break;
					case S::Ready:
						Ayu::Ocr::DeleteLang(code);
						break;
					default:
						Ayu::Ocr::DownloadLang(code);
						break;
					}
				},
			});
		}
	}, ocrExpanded->value() | rpl::map([ocrExpanded](bool v) {
		return v;
	}));

	// FurryGram: local CLIP image search — offline "find photos by text" model.
	// Launched from a chat's "..." menu -> "Search images by text"; this is just
	// the one-time model download / delete.
	const auto clipExpanded = std::make_shared<rpl::variable<bool>>(false);
	builder.addButton({
		.id = u"furry/clipSearch"_q,
		.title = rpl::single(FurryLang::Pick(
			u"Image search (CLIP)"_q,
			QString::fromUtf8("\xd0\x9f\xd0\xbe\xd0\xb8\xd1\x81\xd0\xba\x20\xd0\xba\xd0\xb0\xd1\x80\xd1\x82\xd0\xb8\xd0\xbd\xd0\xbe\xd0\xba\x20\x28\x43\x4c\x49\x50\x29"))),
		.st = &st::settingsButtonNoIcon,
		.onClick = [=] {
			*clipExpanded = !clipExpanded->current();
		},
	});
	builder.scope([&] {
		Ayu::Clip::RefreshModelStatus();

		// Model quality picker. L/14 is much more accurate but heavier and
		// slower on CPU; switching frees the old model and needs a re-index.
		ayu.addChooseButton({
			.id = u"furry/clipModel"_q,
			.title = rpl::single(FurryLang::Pick(
				u"Model"_q,
				QString::fromUtf8("\xd0\x9c\xd0\xbe\xd0\xb4\xd0\xb5\xd0\xbb\xd1\x8c"))),
			.boxTitle = rpl::single(FurryLang::Pick(
				u"CLIP model"_q,
				QString::fromUtf8("\xd0\x9c\xd0\xbe\xd0\xb4\xd0\xb5\xd0\xbb\xd1\x8c\x20\x43\x4c\x49\x50"))),
			.initialSelection = (Ayu::Clip::CurrentModel() == u"l14"_q)
				? 1
				: 0,
			.options = {
				FurryLang::Pick(
					u"Fast (ViT-B/32, ~290 MB)"_q,
					QString::fromUtf8("\xd0\x91\xd1\x8b\xd1\x81\xd1\x82\xd1\x80\xd0\xb0\xd1\x8f\x20\x28\x56\x69\x54\x2d\x42\x2f\x33\x32\x2c\x20\x7e\x32\x39\x30\x20\xd0\x9c\xd0\x91\x29")),
				FurryLang::Pick(
					u"Accurate (ViT-L/14, ~860 MB, slower)"_q,
					QString::fromUtf8("\xd0\xa2\xd0\xbe\xd1\x87\xd0\xbd\xd0\xb0\xd1\x8f\x20\x28\x56\x69\x54\x2d\x4c\x2f\x31\x34\x2c\x20\x7e\x38\x36\x30\x20\xd0\x9c\xd0\x91\x2c\x20\xd0\xbc\xd0\xb5\xd0\xb4\xd0\xbb\xd0\xb5\xd0\xbd\xd0\xbd\xd0\xb5\xd0\xb5\x29")),
			},
			.setter = [](int i) {
				Ayu::Clip::SetModel(i == 1 ? u"l14"_q : u"b32"_q);
			},
		});

		builder.addButton({
			.id = u"furry/clipModelDownload"_q,
			.title = Ayu::Clip::ModelStatusValue(
			) | rpl::map([](Ayu::Clip::ModelProgress p) {
				using S = Ayu::Clip::ModelStatus;
				const auto size = (Ayu::Clip::CurrentModel() == u"l14"_q)
					? QString("~860 MB")
					: QString("~290 MB");
				switch (p.status) {
				case S::Ready:
					return FurryLang::Pick(
						u"Model downloaded - tap to delete"_q,
						QString::fromUtf8("\xd0\x9c\xd0\xbe\xd0\xb4\xd0\xb5\xd0\xbb\xd1\x8c\x20\xd0\xb7\xd0\xb0\xd0\xb3\xd1\x80\xd1\x83\xd0\xb6\xd0\xb5\xd0\xbd\xd0\xb0\x20\xe2\x80\x94\x20\xd0\xbd\xd0\xb0\xd0\xb6\xd0\xbc\xd0\xb8\xd1\x82\xd0\xb5\x2c\x20\xd1\x87\xd1\x82\xd0\xbe\xd0\xb1\xd1\x8b\x20\xd1\x83\xd0\xb4\xd0\xb0\xd0\xbb\xd0\xb8\xd1\x82\xd1\x8c"));
				case S::Downloading:
					return FurryLang::Pick(
						u"Downloading model... %1%"_q,
						QString::fromUtf8("\xd0\x97\xd0\xb0\xd0\xb3\xd1\x80\xd1\x83\xd0\xb7\xd0\xba\xd0\xb0\x20\xd0\xbc\xd0\xbe\xd0\xb4\xd0\xb5\xd0\xbb\xd0\xb8\xe2\x80\xa6\x20\x25\x31\x25")).arg(p.percent);
				default:
					return FurryLang::Pick(
						u"Download model (%1)"_q,
						QString::fromUtf8("\xd0\xa1\xd0\xba\xd0\xb0\xd1\x87\xd0\xb0\xd1\x82\xd1\x8c\x20\xd0\xbc\xd0\xbe\xd0\xb4\xd0\xb5\xd0\xbb\xd1\x8c\x20\x28\x25\x31\x29")).arg(size);
				}
			}),
			.st = &st::settingsButtonNoIcon,
			.onClick = [] {
				using S = Ayu::Clip::ModelStatus;
				switch (Ayu::Clip::CurrentModelStatus()) {
				case S::Downloading:
					Ayu::Clip::CancelModelDownload();
					break;
				case S::Ready:
					Ayu::Clip::DeleteModel();
					break;
				default:
					Ayu::Clip::StartModelDownload();
					break;
				}
			},
		});
	}, clipExpanded->value() | rpl::map([clipExpanded](bool v) {
		return v;
	}));
}

const auto kMeta = BuildHelper({
	.id = AyuGhost::Id(),
	.parentId = AyuMain::Id(),
	.title = u"FurryGram"_q,
	.icon = &st::menuIconGroupReactions,
}, [](SectionBuilder &builder) {
	auto ayu = AyuSectionBuilder(builder);

	builder.addSkip();
	BuildGhostEssentials(builder);

	builder.addSkip();
	BuildSpyEssentials(builder, ayu);

	ayu.addSectionDivider();
	BuildOther(builder, ayu);
	builder.addSkip();
});

} // namespace

rpl::producer<QString> AyuGhost::title() {
	return rpl::single(QString("FurryGram"));
}

AyuGhost::AyuGhost(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller)
, _controller(controller) {
	setupContent();
}

void AyuGhost::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

Type AyuGhostId() {
	return AyuGhost::Id();
}

} // namespace Settings
