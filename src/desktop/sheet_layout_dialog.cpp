#include "sketch/desktop/sheet_layout_dialog.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace sketch::desktop {
namespace {
QString text(const std::string& value) { return QString::fromStdString(value); }
QString number(double value) { return QString::number(value, 'g', 17); }
}

struct SheetLayoutDialog::Impl {
    SheetViewModel model;
    std::optional<SheetViewModel> accepted;
    QComboBox* sheets{};
    QComboBox* placements{};
    QComboBox* views{};
    std::array<QLineEdit*, 4> bounds{};
    QLineEdit* scale{};
    QLabel* target{};
    QLabel* error{};
    QDialogButtonBox* buttons{};
    int sheet_index{-1};
    int placement_index{-1};

    explicit Impl(const SheetViewModel& source) : model(source) {}

    const DrawingSheet* sheet() const {
        if (sheet_index < 0) return nullptr;
        const auto id = sheets->itemData(sheet_index).toString().toStdString();
        const auto found = std::find_if(model.sheets().begin(), model.sheets().end(),
            [&](const auto& candidate) { return candidate.id == id; });
        return found == model.sheets().end() ? nullptr : &*found;
    }
    bool isViewport() const {
        return placement_index >= 0 && placements->itemData(placement_index, Qt::UserRole + 1).toBool();
    }
    std::string placementId() const {
        return placements->itemData(placement_index).toString().toStdString();
    }
    void loadPlacement() {
        error->clear();
        const auto* current = sheet();
        const bool active = current && placement_index >= 0;
        for (auto* field : bounds) { field->setEnabled(active); field->clear(); }
        views->setEnabled(active && isViewport());
        scale->setEnabled(active && isViewport());
        scale->clear();
        views->setCurrentIndex(-1);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(active);
        buttons->button(QDialogButtonBox::Apply)->setEnabled(active);
        if (!current) {
            target->setText(QStringLiteral("Choose a sheet to edit its layout."));
            return;
        }
        target->setText(QStringLiteral("Sheet %1 [%2] — %3 × %4 mm")
            .arg(text(current->number), text(current->id), number(current->width_mm), number(current->height_mm)));
        if (!active) return;
        SheetRect rect;
        const auto id = placementId();
        if (isViewport()) {
            const auto found = std::find_if(current->viewports.begin(), current->viewports.end(),
                [&](const auto& candidate) { return candidate.id == id; });
            if (found == current->viewports.end()) return;
            rect = found->bounds;
            scale->setText(number(found->scale_denominator));
            views->setCurrentIndex(views->findData(text(found->view_id)));
        } else {
            const auto found = std::find_if(current->schedules.begin(), current->schedules.end(),
                [&](const auto& candidate) { return candidate.id == id; });
            if (found == current->schedules.end()) return;
            rect = found->bounds;
        }
        bounds[0]->setText(number(rect.x_mm)); bounds[1]->setText(number(rect.y_mm));
        bounds[2]->setText(number(rect.width_mm)); bounds[3]->setText(number(rect.height_mm));
        target->setText(target->text() + QStringLiteral("\nEditing %1 [%2]")
            .arg(isViewport() ? QStringLiteral("viewport") : QStringLiteral("schedule placement"), text(id)));
    }
    void loadSheet() {
        QSignalBlocker block(placements);
        placements->clear();
        placement_index = -1;
        if (const auto* current = sheet()) {
            for (const auto& viewport : current->viewports) {
                placements->addItem(QStringLiteral("Viewport %1 — view %2").arg(text(viewport.id), text(viewport.view_id)),
                                    text(viewport.id));
                placements->setItemData(placements->count() - 1, true, Qt::UserRole + 1);
            }
            for (const auto& schedule : current->schedules) {
                placements->addItem(QStringLiteral("Schedule placement %1 — %2").arg(text(schedule.id), text(schedule.schedule_id)),
                                    text(schedule.id));
                placements->setItemData(placements->count() - 1, false, Qt::UserRole + 1);
            }
        }
        placements->setCurrentIndex(-1);
        placements->setEnabled(placements->count() > 0);
        loadPlacement();
        if (sheet() && placements->count() == 0)
            target->setText(target->text() + QStringLiteral("\nThis sheet has no viewport or schedule placements."));
    }
    double read(QLineEdit* input, const QString& label) const {
        bool ok = false;
        const double value = input->text().trimmed().toDouble(&ok);
        if (!ok || !std::isfinite(value))
            throw std::invalid_argument((label + QStringLiteral(" must be a finite number (use a decimal point).")).toStdString());
        return value;
    }
    bool apply() {
        try {
            const auto* current = sheet();
            if (!current || placement_index < 0) throw std::invalid_argument("Choose a sheet and a placement.");
            const auto id = placementId();
            const auto sheet_id = current->id;
            const SheetRect rect{read(bounds[0], "X"), read(bounds[1], "Y"),
                                 read(bounds[2], "Width"), read(bounds[3], "Height")};
            if (isViewport()) {
                const auto found = std::find_if(current->viewports.begin(), current->viewports.end(),
                    [&](const auto& candidate) { return candidate.id == id; });
                if (found == current->viewports.end()) throw std::invalid_argument("Viewport no longer exists.");
                auto replacement = *found;
                replacement.bounds = rect;
                replacement.scale_denominator = read(scale, "Scale denominator");
                replacement.view_id = views->currentData().toString().toStdString();
                model = model.with_viewport(sheet_id, std::move(replacement));
            } else {
                const auto found = std::find_if(current->schedules.begin(), current->schedules.end(),
                    [&](const auto& candidate) { return candidate.id == id; });
                if (found == current->schedules.end()) throw std::invalid_argument("Schedule placement no longer exists.");
                auto replacement = *found;
                replacement.bounds = rect;
                model = model.with_schedule_placement(sheet_id, std::move(replacement));
            }
            error->clear();
            if (isViewport()) {
                placements->setItemText(placement_index, QStringLiteral("Viewport %1 — view %2")
                    .arg(text(id), views->currentData().toString()));
            }
            return true;
        } catch (const std::exception& exception) {
            error->setText(QString::fromUtf8(exception.what()));
            return false;
        }
    }
    bool changeSheet(int index) {
        if (index == sheet_index) return true;
        if (placement_index >= 0 && !apply()) return false;
        sheet_index = index;
        { QSignalBlocker block(sheets); sheets->setCurrentIndex(index); }
        loadSheet();
        return true;
    }
    bool changePlacement(int index) {
        if (index == placement_index) return true;
        if (placement_index >= 0 && !apply()) return false;
        placement_index = index;
        { QSignalBlocker block(placements); placements->setCurrentIndex(index); }
        loadPlacement();
        return true;
    }
};

SheetLayoutDialog::SheetLayoutDialog(const SheetViewModel& model, const QString& selected_sheet_id,
                                   QWidget* parent)
    : QDialog(parent), impl_(std::make_unique<Impl>(model)) {
    setWindowTitle(QStringLiteral("Sheet layout manager"));
    setObjectName(QStringLiteral("sheetLayoutDialog"));
    resize(640, 490);
    auto& p = *impl_;
    auto* layout = new QVBoxLayout(this);
    auto* help = new QLabel(QStringLiteral("Choose the exact sheet and placement. Valid edits are staged when you switch targets. "
        "OK saves all staged edits; Cancel discards them. Coordinates and sizes are paper millimetres, measured from the top-left."), this);
    help->setWordWrap(true); layout->addWidget(help);
    auto* form = new QFormLayout;
    p.sheets = new QComboBox(this); p.sheets->setObjectName("sheetLayoutSheet");
    p.sheets->setPlaceholderText(QStringLiteral("Choose a sheet"));
    for (const auto& sheet : model.sheets())
        p.sheets->addItem(QStringLiteral("%1 — %2 [%3]").arg(text(sheet.number), text(sheet.title_block.title), text(sheet.id)), text(sheet.id));
    p.sheets->setCurrentIndex(-1);
    form->addRow(QStringLiteral("&Sheet"), p.sheets);
    p.placements = new QComboBox(this); p.placements->setObjectName("sheetLayoutPlacement");
    p.placements->setPlaceholderText(QStringLiteral("Choose a viewport or schedule placement"));
    form->addRow(QStringLiteral("&Placement"), p.placements);
    p.target = new QLabel(this); p.target->setObjectName("sheetLayoutTarget"); p.target->setWordWrap(true);
    form->addRow(p.target);
    p.views = new QComboBox(this); p.views->setObjectName("sheetLayoutView");
    for (const auto& view : model.views())
        p.views->addItem(QStringLiteral("%1 [%2]").arg(text(view.name), text(view.id)), text(view.id));
    form->addRow(QStringLiteral("Assigned &view"), p.views);
    const std::array<const char*, 4> names{"sheetLayoutX", "sheetLayoutY", "sheetLayoutWidth", "sheetLayoutHeight"};
    const std::array<QString, 4> labels{QStringLiteral("&X (mm)"), QStringLiteral("&Y (mm)"),
                                      QStringLiteral("&Width (mm)"), QStringLiteral("&Height (mm)")};
    for (std::size_t i = 0; i < p.bounds.size(); ++i) {
        p.bounds[i] = new QLineEdit(this); p.bounds[i]->setObjectName(names[i]);
        form->addRow(labels[i], p.bounds[i]);
    }
    p.scale = new QLineEdit(this); p.scale->setObjectName("sheetLayoutScale");
    form->addRow(QStringLiteral("Scale 1 : (&denominator)"), p.scale);
    layout->addLayout(form);
    p.error = new QLabel(this); p.error->setObjectName("sheetLayoutError"); p.error->setWordWrap(true);
    layout->addWidget(p.error);
    p.buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, this);
    p.buttons->button(QDialogButtonBox::Apply)->setText(QStringLiteral("Stage edit"));
    layout->addWidget(p.buttons);
    connect(p.buttons, &QDialogButtonBox::accepted, this, &SheetLayoutDialog::accept);
    connect(p.buttons, &QDialogButtonBox::rejected, this, &SheetLayoutDialog::reject);
    connect(p.buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { (void)applyCurrentEdit(); });
    connect(p.sheets, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!impl_->changeSheet(index)) { QSignalBlocker block(impl_->sheets); impl_->sheets->setCurrentIndex(impl_->sheet_index); }
    });
    connect(p.placements, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!impl_->changePlacement(index)) { QSignalBlocker block(impl_->placements); impl_->placements->setCurrentIndex(impl_->placement_index); }
    });
    p.loadSheet();
    if (!selected_sheet_id.isEmpty()) (void)selectSheet(selected_sheet_id);
}

SheetLayoutDialog::~SheetLayoutDialog() = default;
bool SheetLayoutDialog::selectSheet(const QString& id) {
    const int index = impl_->sheets->findData(id);
    return index >= 0 && impl_->changeSheet(index);
}
bool SheetLayoutDialog::selectViewport(const QString& id) {
    for (int i = 0; i < impl_->placements->count(); ++i)
        if (impl_->placements->itemData(i).toString() == id && impl_->placements->itemData(i, Qt::UserRole + 1).toBool())
            return impl_->changePlacement(i);
    return false;
}
bool SheetLayoutDialog::selectSchedulePlacement(const QString& id) {
    for (int i = 0; i < impl_->placements->count(); ++i)
        if (impl_->placements->itemData(i).toString() == id && !impl_->placements->itemData(i, Qt::UserRole + 1).toBool())
            return impl_->changePlacement(i);
    return false;
}
QString SheetLayoutDialog::selectedSheetId() const {
    return impl_->sheet() ? text(impl_->sheet()->id) : QString{};
}
const SheetViewModel& SheetLayoutDialog::workingModel() const { return impl_->model; }
const std::optional<SheetViewModel>& SheetLayoutDialog::acceptedModel() const { return impl_->accepted; }
bool SheetLayoutDialog::applyCurrentEdit() { return impl_->apply(); }
void SheetLayoutDialog::accept() {
    if (!applyCurrentEdit()) return;
    impl_->accepted = impl_->model;
    QDialog::accept();
}
void SheetLayoutDialog::reject() {
    impl_->accepted.reset();
    QDialog::reject();
}
} // namespace sketch::desktop
