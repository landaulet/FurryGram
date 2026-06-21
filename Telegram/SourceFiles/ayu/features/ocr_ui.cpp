// FurryGram: OCR result UI (kept separate from the Tesseract engine TU).
#include "ayu/features/ocr.h"

#include "lang/lang_keys.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h" // Ui::Show
#include "ui/text/text_entity.h" // TextForMimeData
#include "ui/text/text_utilities.h" // TextUtilities::SetClipboardText
#include "ui/widgets/labels.h"
#include "styles/style_layers.h"

namespace Ayu::Ocr {

void ShowResultBox(
		std::shared_ptr<Ui::Show> show,
		const QString &text) {
	if (!show) {
		return;
	}
	const auto body = text.trimmed().isEmpty()
		? QString("(no text recognized)")
		: text;
	show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(QString("Recognized text")));
		const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(body),
			st::boxLabel));
		label->setSelectable(true);
		box->addButton(rpl::single(QString("Copy")), [=] {
			TextUtilities::SetClipboardText(TextForMimeData::Simple(body));
			box->uiShow()->showToast(tr::lng_text_copied(tr::now));
		});
		box->addButton(tr::lng_close(), [=] {
			box->closeBox();
		});
	}));
}

} // namespace Ayu::Ocr
