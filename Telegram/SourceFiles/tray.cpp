/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "tray.h"
#include "tray_accounts_menu.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "platform/platform_notifications_manager.h"
#include "platform/platform_specific.h"
#include "lang/lang_keys.h"

#include <QtWidgets/QApplication>

// AyuGram includes
#include "ayu/ayu_settings.h"
#include "ayu/features/furry_lang.h"
#include "ayu/features/streamer_mode/streamer_mode.h"
#include "window/window_controller.h"
#include "lang_auto.h"


namespace Core {

Tray::Tray() {
}

void Tray::create() {
	rebuildMenu();
	using WorkMode = Settings::WorkMode;
	if (Core::App().settings().workMode() != WorkMode::WindowOnly) {
		_tray.createIcon();
	}

	Core::App().settings().workModeValue(
	) | rpl::combine_previous(
	) | rpl::on_next([=](WorkMode previous, WorkMode state) {
		const auto wasHasIcon = (previous != WorkMode::WindowOnly);
		const auto nowHasIcon = (state != WorkMode::WindowOnly);
		if (wasHasIcon != nowHasIcon) {
			if (nowHasIcon) {
				_tray.createIcon();
			} else {
				_tray.destroyIcon();
			}
		}
	}, _tray.lifetime());

	Core::App().settings().trayIconMonochromeChanges(
	) | rpl::on_next([=] {
		updateIconCounters();
	}, _tray.lifetime());

	Core::App().passcodeLockChanges(
	) | rpl::on_next([=] {
		rebuildMenu();
	}, _tray.lifetime());

	TrayAccountsMenu::SetupChangesSubscription(
		[=] { rebuildMenu(); },
		_tray.lifetime());

	_tray.iconClicks(
	) | rpl::on_next([=] {
		const auto skipTrayClick = (_lastTrayClickTime > 0)
			&& (crl::now() - _lastTrayClickTime
				< QApplication::doubleClickInterval());
		if (!skipTrayClick) {
			_activeForTrayIconAction = Core::App().isActiveForTrayMenu();
			_minimizeMenuItemClicks.fire({});
			_lastTrayClickTime = crl::now();
		}
	}, _tray.lifetime());
}

void Tray::rebuildMenu() {
	_tray.destroyMenu();
	_tray.createMenu();

	{
		auto minimizeText = _textUpdates.events(
		) | rpl::map([=] {
			_activeForTrayIconAction = Core::App().isActiveForTrayMenu();
			return _activeForTrayIconAction
				? tr::lng_minimize_to_tray(tr::now)
				: tr::lng_open_from_tray(tr::now).replace("Telegram", "FurryGram");
		});

		_tray.addAction(
			std::move(minimizeText),
			[=] { _minimizeMenuItemClicks.fire({}); });
	}

	if (!Core::App().passcodeLocked()) {
		auto notificationsText = _textUpdates.events(
		) | rpl::map([=] {
			return Core::App().settings().desktopNotify()
				? tr::lng_disable_notifications_from_tray(tr::now)
				: tr::lng_enable_notifications_from_tray(tr::now);
		});

		_tray.addAction(
			std::move(notificationsText),
			[=] { toggleSoundNotifications(); });
	}

	const auto &settings = AyuSettings::getInstance();

	if (settings.showGhostToggleInTray()) {
		auto ghostActiveChanges = AyuSettings::getInstance().useGlobalGhostModeValue()
			| rpl::map([](bool) {
				return AyuSettings::ghost().ghostModeActiveValue();
			})
			| rpl::flatten_latest();

		auto turnGhostModeText = rpl::combine(
			_textUpdates.events_starting_with({}),
			std::move(ghostActiveChanges)
		) | rpl::map([=](auto, bool active) {
			return active
				? tr::ayu_DisableGhostModeTray(tr::now)
				: tr::ayu_EnableGhostModeTray(tr::now);
		});
		_tray.addAction(
			std::move(turnGhostModeText),
			[=]
			{
				auto &ghost = AyuSettings::ghost();
				ghost.setGhostModeEnabled(!ghost.isGhostModeActive());
			});
	}

	if (settings.showStreamerToggleInTray()) {
		auto turnStreamerModeText = _textUpdates.events(
		) | rpl::map(
			[=]
			{
				bool streamerModeEnabled = AyuFeatures::StreamerMode::isEnabled();

				return streamerModeEnabled
						   ? tr::ayu_DisableStreamerModeTray(tr::now)
						   : tr::ayu_EnableStreamerModeTray(tr::now);
			});
		_tray.addAction(
			std::move(turnStreamerModeText),
			[=]
			{
				if (AyuFeatures::StreamerMode::isEnabled()) {
					AyuFeatures::StreamerMode::disable();
				} else {
					AyuFeatures::StreamerMode::enable();
				}
			});
	}

	// FurryGram: Focus mode quick toggle from the tray.
	{
		auto turnFocusText = rpl::combine(
			_textUpdates.events_starting_with({}),
			AyuSettings::getInstance().focusEnabledValue()
		) | rpl::map([=](auto, bool active) {
			return active
				? FurryLang::Pick(
					u"Disable Focus"_q,
					QString::fromUtf8("\xD0\x92\xD1\x8B\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB8\xD1\x82\xD1\x8C\x20\xD1\x84\xD0\xBE\xD0\xBA\xD1\x83\xD1\x81"))
				: FurryLang::Pick(
					u"Enable Focus"_q,
					QString::fromUtf8("\xD0\x92\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB8\xD1\x82\xD1\x8C\x20\xD1\x84\xD0\xBE\xD0\xBA\xD1\x83\xD1\x81"));
		});
		_tray.addAction(
			std::move(turnFocusText),
			[=]
			{
				auto &focus = AyuSettings::getInstance();
				focus.setFocusEnabled(!focus.focusEnabled());
			});
	}

	auto quitText = _textUpdates.events(
	) | rpl::map([=]
	{
		return tr::lng_quit_from_tray(tr::now).replace("Telegram", "FurryGram");
	});
	_tray.addAction(std::move(quitText), [] { Core::Quit(); });

	TrayAccountsMenu::Fill(_tray);

	updateMenuText();
}

void Tray::updateMenuText() {
	_textUpdates.fire({});
}

void Tray::updateIconCounters() {
	_tray.updateIcon();
}

rpl::producer<> Tray::aboutToShowRequests() const {
	return _tray.aboutToShowRequests();
}

rpl::producer<> Tray::showFromTrayRequests() const {
	return rpl::merge(
		_tray.showFromTrayRequests(),
		_minimizeMenuItemClicks.events() | rpl::filter([=] {
			return !_activeForTrayIconAction;
		})
	);
}

rpl::producer<> Tray::hideToTrayRequests() const {
	auto triggers = rpl::merge(
		_tray.hideToTrayRequests(),
		_minimizeMenuItemClicks.events() | rpl::filter([=] {
			return _activeForTrayIconAction;
		})
	);
	if (_tray.hasTrayMessageSupport()) {
		return std::move(triggers) | rpl::map([=]() -> rpl::empty_value {
			_tray.showTrayMessage();
			return {};
		});
	} else {
		return triggers;
	}
}

void Tray::toggleSoundNotifications() {
	auto soundNotifyChanged = false;
	auto flashBounceNotifyChanged = false;
	auto &settings = Core::App().settings();
	settings.setDesktopNotify(!settings.desktopNotify());
	if (settings.desktopNotify()) {
		if (settings.rememberedSoundNotifyFromTray()
			&& !settings.soundNotify()) {
			settings.setSoundNotify(true);
			settings.setRememberedSoundNotifyFromTray(false);
			soundNotifyChanged = true;
		}
		if (settings.rememberedFlashBounceNotifyFromTray()
			&& !settings.flashBounceNotify()) {
			settings.setFlashBounceNotify(true);
			settings.setRememberedFlashBounceNotifyFromTray(false);
			flashBounceNotifyChanged = true;
		}
	} else {
		if (settings.soundNotify()) {
			settings.setSoundNotify(false);
			settings.setRememberedSoundNotifyFromTray(true);
			soundNotifyChanged = true;
		} else {
			settings.setRememberedSoundNotifyFromTray(false);
		}
		if (settings.flashBounceNotify()) {
			settings.setFlashBounceNotify(false);
			settings.setRememberedFlashBounceNotifyFromTray(true);
			flashBounceNotifyChanged = true;
		} else {
			settings.setRememberedFlashBounceNotifyFromTray(false);
		}
	}
	Core::App().saveSettingsDelayed();
	using Change = Window::Notifications::ChangeType;
	auto &notifications = Core::App().notifications();
	notifications.notifySettingsChanged(Change::DesktopEnabled);
	if (soundNotifyChanged) {
		notifications.notifySettingsChanged(Change::SoundEnabled);
	}
	if (flashBounceNotifyChanged) {
		notifications.notifySettingsChanged(Change::FlashBounceEnabled);
	}
}

bool Tray::has() const {
	return _tray.hasIcon() && Platform::TrayIconSupported();
}

} // namespace Core
