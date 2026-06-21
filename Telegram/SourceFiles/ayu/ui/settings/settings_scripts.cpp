// FurryGram scripts settings section.
#include "ayu/ui/settings/settings_scripts.h"

#include "lang_auto.h"
#include "ayu/scripting/furry_scripts.h"
#include "ayu/ui/settings/settings_main.h"
#include "settings/settings_builder.h"
#include "settings/settings_common.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/layers/generic_box.h"
#include "lang/lang_keys.h"
#include "window/window_session_controller.h"

#include <QtGui/QDesktopServices>
#include <QtCore/QUrl>

namespace Settings {

using namespace Builder;

namespace {

// Per-script settings popup: renders the options a script declared via
// furry.registerSettings (toggle/number/text) and persists changes.
void ScriptSettingsBox(
		not_null<Ui::GenericBox*> box,
		const QString &name) {
	const auto schema = FurryScripts::ScriptSchema(name);
	box->setTitle(rpl::single(schema.title.isEmpty() ? name : schema.title));

	const auto container = box->verticalLayout();

	// Master enable/disable for the whole script (first row). Read state from the
	// synchronous source (IsScriptEnabled) and act only on a real change vs. the
	// current state (not a captured snapshot) so repeated toggles work.
	{
		const auto button = container->add(object_ptr<Ui::SettingsButton>(
			container,
			rpl::single(QString("Enabled")),
			st::settingsButtonNoIcon));
		button->toggleOn(rpl::single(FurryScripts::IsScriptEnabled(name)));
		button->toggledValue(
		) | rpl::filter([=](bool on) {
			return on != FurryScripts::IsScriptEnabled(name);
		}) | rpl::on_next([=](bool on) {
			FurryScripts::SetScriptEnabled(name, on);
		}, button->lifetime());
		Ui::AddDivider(container);
	}

	for (const auto &opt : schema.options) {
		if (opt.type == u"toggle"_q) {
			const auto button = container->add(object_ptr<Ui::SettingsButton>(
				container,
				rpl::single(opt.label),
				st::settingsButtonNoIcon));
			button->toggleOn(rpl::single(
				FurryScripts::GetScriptBool(name, opt.key, opt.defBool)));
			const auto key = opt.key;
			button->toggledValue(
			) | rpl::on_next([=](bool v) {
				FurryScripts::SetScriptBool(name, key, v);
			}, button->lifetime());
		} else {
			// number + text both use a text input (number parsed on change).
			Ui::AddSubsectionTitle(container, rpl::single(opt.label));
			const auto initial = (opt.type == u"number"_q)
				? QString::number(FurryScripts::GetScriptNumber(name, opt.key, opt.defNumber))
				: FurryScripts::GetScriptText(name, opt.key, opt.defString);
			const auto field = container->add(
				object_ptr<Ui::InputField>(
					container,
					st::defaultInputField,
					rpl::single(opt.label),
					initial),
				st::boxRowPadding);
			const auto key = opt.key;
			const auto isNumber = (opt.type == u"number"_q);
			field->changes(
			) | rpl::on_next([=] {
				const auto text = field->getLastText();
				if (isNumber) {
					FurryScripts::SetScriptNumber(name, key, text.toDouble());
				} else {
					FurryScripts::SetScriptText(name, key, text);
				}
			}, field->lifetime());
		}
	}
	if (schema.options.empty()) {
		Ui::AddSubsectionTitle(
			container,
			rpl::single(QString("This script has no settings.")));
	}
	box->addButton(tr::lng_box_done(), [=] { box->closeBox(); });
}

void BuildScriptsList(SectionBuilder &builder) {
	builder.addSkip();
	builder.addSubsectionTitle(rpl::single(QString("Loaded scripts")));

	const auto scripts = FurryScripts::LoadedScripts();
	if (scripts.empty()) {
		builder.addButton({
			.id = u"furry/scripts/none"_q,
			.title = rpl::single(QString("No scripts found")),
			.st = &st::settingsButtonNoIcon,
		});
	} else {
		const auto controller = builder.controller();
		for (auto i = 0; i != int(scripts.size()); ++i) {
			const auto &s = scripts[i];
			const auto name = s.name;
			const auto ok = s.ok;
			// Clicking the row opens the script's settings popup (which also holds
			// the enable/disable toggle). We DON'T put a toggle on the row itself:
			// a tdesktop button can't have both an independent toggle and a click
			// action — the body click would flip the toggle. The label shows state
			// and updates live (ScriptsChanged) when toggled from the popup.
			auto label = rpl::single(rpl::empty) | rpl::then(
				FurryScripts::ScriptsChanged()
			) | rpl::map([=] {
				const auto on = FurryScripts::IsScriptEnabled(name);
				return on ? (ok ? QString("on") : QString("error")) : QString("off");
			});
			builder.addButton({
				.id = u"furry/scripts/item/"_q + name,
				.title = rpl::single(name),
				.icon = IconDescriptor{ &st::menuIconEdit },
				.label = std::move(label),
				.onClick = Fn<void()>([=] {
					controller->show(Box(ScriptSettingsBox, name));
				}),
			});
		}
	}

	builder.addSkip();
}

void BuildScriptsActions(SectionBuilder &builder) {
	builder.addDivider();
	builder.addSkip();

	builder.addButton({
		.id = u"furry/scripts/open"_q,
		.title = rpl::single(QString("Open scripts folder")),
		.icon = { &st::menuIconShowInChat },
		.onClick = [] {
			const auto path = FurryScripts::ScriptsFolder();
			QDesktopServices::openUrl(QUrl::fromLocalFile(path));
		},
	});

	const auto controller = builder.controller();
	builder.addButton({
		.id = u"furry/scripts/reload"_q,
		.title = rpl::single(QString("Reload scripts")),
		.icon = { &st::menuIconRestore },
		.onClick = [=] {
			FurryScripts::Reload();
			// Re-open the section to refresh the displayed list.
			controller->showSettings(AyuScripts::Id());
		},
	});

	builder.addSkip();
}

const auto kMeta = BuildHelper({
	.id = AyuScripts::Id(),
	.parentId = AyuMain::Id(),
	.title = QString("Scripts"),
	.icon = &st::menuIconEdit,
}, [](SectionBuilder &builder) {
	BuildScriptsList(builder);
	BuildScriptsActions(builder);
});

} // namespace

rpl::producer<QString> AyuScripts::title() {
	return rpl::single(QString("Scripts"));
}

AyuScripts::AyuScripts(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

void AyuScripts::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, kMeta.build);
	Ui::ResizeFitChild(this, content);
}

Type AyuScriptsId() {
	return AyuScripts::Id();
}

} // namespace Settings
