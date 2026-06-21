// FurryGram: chat picker for Focus "always notify" exceptions.
#include "ayu/ui/settings/focus_exceptions_box.h"

#include "ayu/ayu_settings.h"
#include "ayu/features/furry_lang.h"
#include "boxes/abstract_box.h"
#include "boxes/peer_list_box.h"
#include "boxes/peer_list_controllers.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"

#include "base/flat_set.h"

namespace Ayu::Focus {
namespace {

class ExceptionsController final : public ChatsListBoxController {
public:
	ExceptionsController(
		not_null<Main::Session*> session,
		std::vector<not_null<History*>> selected)
	: ChatsListBoxController(session)
	, _session(session)
	, _selected(std::move(selected)) {
	}

	Main::Session &session() const override {
		return *_session;
	}

	void rowClicked(not_null<PeerListRow*> row) override {
		delegate()->peerListSetRowChecked(row, !row->checked());
	}

protected:
	std::unique_ptr<Row> createRow(not_null<History*> history) override {
		return history->inChatList()
			? std::make_unique<Row>(history)
			: nullptr;
	}

	void prepareViewHook() override {
		delegate()->peerListSetTitle(rpl::single(FurryLang::Pick(
			u"Always notify in Focus"_q,
			QString::fromUtf8("\xD0\x92\xD1\x81\xD0\xB5\xD0\xB3\xD0\xB4\xD0\xB0\x20\xD1\x83\xD0\xB2\xD0\xB5\xD0\xB4\xD0\xBE\xD0\xBC\xD0\xBB\xD1\x8F\xD1\x82\xD1\x8C\x20\xD0\xB2\x20\xD1\x84\xD0\xBE\xD0\xBA\xD1\x83\xD1\x81\xD0\xB5"))));

		const auto count = int(_selected.size());
		auto rows = std::make_unique<std::optional<Row>[]>(count);
		for (auto i = 0; i != count; ++i) {
			rows[i].emplace(_selected[i]);
		}
		auto pointers = std::vector<Row*>();
		pointers.reserve(count);
		for (auto i = 0; i != count; ++i) {
			pointers.push_back(&*rows[i]);
		}
		delegate()->peerListAddSelectedRows(pointers);
	}

private:
	const not_null<Main::Session*> _session;
	std::vector<not_null<History*>> _selected;

};

} // namespace

void ShowExceptionsBox(not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();

	auto selected = std::vector<not_null<History*>>();
	for (const auto id : AyuSettings::getInstance().focusExceptions()) {
		const auto peerId = PeerId(uint64(id));
		if (const auto peer = session->data().peerLoaded(peerId)) {
			selected.push_back(session->data().history(peer));
		}
	}

	auto controllerPtr = std::make_unique<ExceptionsController>(
		session,
		std::move(selected));
	auto initBox = [=](not_null<PeerListBox*> box) {
		box->addButton(rpl::single(FurryLang::Pick(
			u"Save"_q,
			QString::fromUtf8("\xD0\xA1\xD0\xBE\xD1\x85\xD1\x80\xD0\xB0\xD0\xBD\xD0\xB8\xD1\x82\xD1\x8C"))), [=] {
			auto &settings = AyuSettings::getInstance();
			const auto chosen = box->collectSelectedRows();
			auto want = base::flat_set<qint64>();
			for (const auto peer : chosen) {
				want.emplace(qint64(peer->id.value));
			}
			const auto current = settings.focusExceptions(); // copy
			for (const auto id : current) {
				if (!want.contains(id)) {
					settings.setFocusException(id, false);
				}
			}
			for (const auto id : want) {
				settings.setFocusException(id, true);
			}
			box->closeBox();
		});
		box->addButton(tr::lng_cancel(), [box] { box->closeBox(); });
	};
	controller->show(Box<PeerListBox>(
		std::move(controllerPtr),
		std::move(initBox)));
}

} // namespace Ayu::Focus
