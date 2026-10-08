#include "horizontal_layer_stack_dialog.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/document_solid.hpp"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace sketch::desktop {
namespace {
void require(bool value, const char* message) { if (!value) throw std::invalid_argument(message); }
QString q(const std::string& value) { return QString::fromStdString(value); }
QString name(const Entity& entity) {
    const auto found = entity.properties.find("name");
    return found != entity.properties.end() && found->is_string() && !q(found->get<std::string>()).trimmed().isEmpty()
        ? q(found->get<std::string>()) : q(entity.id);
}
QString native_text(double metres) {
    require(std::isfinite(metres), "Thickness must be finite");
    std::array<char, 768> buffer{};
    for (int precision = 0; precision <= 18; ++precision) {
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), metres,
                                          std::chars_format::fixed, precision);
        if (result.ec != std::errc{}) continue;
        std::string text(buffer.data(), result.ptr);
        if (text.find('.') != std::string::npos) {
            while (text.back() == '0') text.pop_back();
            if (text.back() == '.') text.pop_back();
        }
        text += " m";
        try { if (parse_quantity(text, Unit::metre).metres == metres) return q(text); }
        catch (const std::exception&) {}
    }
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), metres);
    require(result.ec == std::errc{}, "The native thickness cannot be displayed");
    return q(std::string(buffer.data(), result.ptr) + " m");
}
QString initial_text(const Entity& entity, const std::vector<std::string>& pointers, double metres, Unit unit) {
    const auto entries = entity.properties.find("quantity_entries");
    if (entries != entity.properties.end()) for (const auto& pointer : pointers) {
        const auto found = entries->find(pointer);
        if (found == entries->end() || !found->is_object() || found->value("version", 0) != 1) continue;
        const auto receipt = decode_constraint_quantity_receipt(*found);
        require(receipt.metres == metres, "The saved layer measurement is stale");
        try { if (parse_quantity(receipt.original_expression, unit).metres == metres) return q(receipt.original_expression); }
        catch (const std::exception&) {}
        return q(format_quantity(receipt, receipt.entered_unit));
    }
    return native_text(metres);
}
} // namespace

class HorizontalLayerStackDialog::Impl final {
public:
    struct Row {
        std::string id;
        bool added{};
        double native_thickness{};
        QString initial;
        QString text;
        std::optional<WallLayerMaterial> original_material;
        int material_index{};
        QLineEdit* field{};
        QComboBox* material{};
    };
    HorizontalLayerStackDialog* owner;
    DocumentSnapshot source;
    std::string slab_id, digest;
    Unit unit;
    std::function<DocumentSnapshot()> current_source;
    std::function<std::string()> allocate_layer_id;
    Entity original;
    std::vector<Row> rows;
    std::vector<std::pair<QString, WallLayerMaterial>> materials;
    QTableWidget* table{};
    QLineEdit* thickness{};
    QString initial_thickness, source_error;
    QLabel* sum{};
    QLabel* error{};
    QPushButton* save{};
    bool stale{}, rebuilding{};
    std::optional<SlabLayerStackEditIntent> accepted_intent;
    std::optional<Entity> accepted_entity;

    Impl(HorizontalLayerStackDialog* dialog, const DocumentSnapshot& captured, std::string id,
         bool metric, std::function<DocumentSnapshot()> provider, std::function<std::string()> allocator)
        : owner(dialog), source(captured), slab_id(std::move(id)), digest(document_snapshot_digest(source)),
          unit(metric ? Unit::metre : Unit::foot), current_source(std::move(provider)), allocate_layer_id(std::move(allocator)) {
        owner->setObjectName("horizontalLayerStackDialog");
        owner->setWindowTitle("Horizontal layers");
        owner->resize(660, 420);
        auto* layout = new QVBoxLayout(owner);
        layout->setSpacing(12);
        auto* heading = new QLabel("Horizontal system", owner);
        heading->setWordWrap(true);
        layout->addWidget(heading);
        thickness = new QLineEdit(owner);
        thickness->setObjectName("horizontalLayerStackThickness");
        configure(thickness);
        auto* form = new QFormLayout;
        form->addRow("Total thickness", thickness);
        layout->addLayout(form);
        layout->addWidget(new QLabel("Layers · bottom to top", owner));
        table = new QTableWidget(0, 3, owner);
        table->setObjectName("horizontalLayerStackRows");
        table->setAccessibleName("Layers ordered from bottom to top");
        table->setHorizontalHeaderLabels({"Layer", "Thickness", "Material / catalog"});
        table->verticalHeader()->hide();
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setAlternatingRowColors(true);
        table->setSortingEnabled(false);
        layout->addWidget(table, 1);
        auto* controls = new QHBoxLayout;
        const auto button = [&](const char* text, auto callback) {
            auto* result = new QPushButton(text, owner);
            controls->addWidget(result);
            QObject::connect(result, &QPushButton::clicked, owner, callback);
            return result;
        };
        button("Add", [this] {
            try {
                fence();
                require(static_cast<bool>(allocate_layer_id), "Layer identity allocation is unavailable");
                const auto id = allocate_layer_id();
                require(!id.empty(), "The new layer identity is empty");
                for (const auto& row : rows) require(row.id != id, "The new layer identity is already used");
                capture();
                Row row; row.id = id; row.added = true;
                rows.push_back(std::move(row));
                rebuild(static_cast<int>(rows.size()) - 1);
                (void)validate();
            } catch (const std::exception& reason) { fail(QString::fromUtf8(reason.what())); }
            catch (...) { fail("A new layer could not be prepared. Reopen these layers."); }
        });
        button("Remove", [this] {
            const int selected = table->currentRow();
            if (selected < 0) return;
            capture(); rows.erase(rows.begin() + selected); rebuild(selected); (void)validate();
        });
        button("Up", [this] { move(-1); });
        button("Down", [this] { move(1); });
        controls->addStretch();
        sum = new QLabel(owner);
        sum->setObjectName("horizontalLayerStackSum");
        controls->addWidget(sum);
        layout->addLayout(controls);
        error = new QLabel(owner);
        error->setObjectName("horizontalLayerStackError");
        error->setWordWrap(true);
        layout->addWidget(error);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, owner);
        save = buttons->button(QDialogButtonBox::Save);
        save->setObjectName("saveHorizontalLayerStack");
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, owner, [this] { (void)owner->submit(); });
        QObject::connect(buttons, &QDialogButtonBox::rejected, owner, &QDialog::reject);
        try {
            require(source.is_editable(), "The captured project is read only");
            require(static_cast<bool>(current_source), "The current project source is unavailable");
            const auto found = source.entities().find(slab_id);
            require(found != source.entities().end() && found->second.id == slab_id, "The horizontal system is unavailable");
            original = found->second;
            Slab slab; std::string diagnostic;
            if (!read_document_slab(original, slab, diagnostic)) throw std::invalid_argument(diagnostic);
            heading->setText(name(original));
            initial_thickness = initial_text(original, {"/thickness_m", "/thickness"}, slab.thickness, unit);
            thickness->setText(initial_thickness);
            for (const auto& [catalog_id, catalog] : source.entities()) {
                if (catalog.type != "assembly_model") continue;
                try {
                    const auto model = AssemblyModel::from_json(catalog.properties.at("model"));
                    for (const auto& material : model.materials())
                        materials.push_back({q(material.name) + " · " + name(catalog), {catalog_id, material.id}});
                } catch (const std::exception&) { /* Unsupported catalogs are not assignable. */ }
            }
            for (std::size_t i = 0; i < slab.layers.size(); ++i) {
                const auto& layer = slab.layers[i];
                Row row; row.id = layer.id; row.native_thickness = layer.thickness;
                row.initial = initial_text(original, {"/layers/" + std::to_string(i) + "/thickness_m"}, layer.thickness, unit);
                row.text = row.initial; row.original_material = layer.material;
                if (layer.material) {
                    bool found_material = false;
                    for (std::size_t m = 0; m < materials.size(); ++m) if (materials[m].second == *layer.material) {
                        row.material_index = static_cast<int>(m) + 1; found_material = true; break;
                    }
                    require(found_material, "An existing layer material is unavailable in its catalog");
                }
                rows.push_back(std::move(row));
            }
            rebuild(-1);
        } catch (const std::exception& reason) {
            source_error = QString::fromUtf8(reason.what());
            table->setEnabled(false);
            thickness->setEnabled(false);
            for (int i = 0; i < controls->count(); ++i)
                if (auto* widget = controls->itemAt(i)->widget()) widget->setEnabled(false);
        }
        QObject::connect(thickness, &QLineEdit::textChanged, owner, [this] { (void)validate(); });
        auto* timer = new QTimer(owner);
        timer->setInterval(250);
        QObject::connect(timer, &QTimer::timeout, owner, [this] {
            if (stale || !source_error.isEmpty()) return;
            try { fence(); } catch (const std::exception& reason) { fail(QString::fromUtf8(reason.what())); }
            catch (...) { fail("The current project source is unavailable. Reopen these layers."); }
        });
        QObject::connect(owner, &QDialog::finished, timer, [this, timer](int result) {
            timer->stop();
            if (result != QDialog::Accepted) { accepted_intent.reset(); accepted_entity.reset(); }
        });
        timer->start();
        (void)validate();
    }
    void configure(QLineEdit* field) {
        field->setMaxLength(4096);
        field->setToolTip(unit == Unit::metre ? "Enter m, mm, cm, or feet and inches. A bare number uses metres."
                                             : "Enter feet and inches, or m, mm, cm. A bare number uses feet.");
    }
    void capture() {
        for (auto& row : rows) { row.text = row.field->text(); row.material_index = row.material->currentIndex(); }
    }
    void rebuild(int selected) {
        rebuilding = true;
        table->clearContents(); table->setRowCount(static_cast<int>(rows.size()));
        for (std::size_t i = 0; i < rows.size(); ++i) {
            auto& row = rows[i]; const int index = static_cast<int>(i);
            auto* item = new QTableWidgetItem(QString("Layer %1").arg(index + 1));
            item->setData(Qt::UserRole, q(row.id));
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            table->setItem(index, 0, item);
            row.field = new QLineEdit(row.text, table); configure(row.field);
            row.field->setAccessibleName(QString("Thickness of layer %1").arg(index + 1));
            table->setCellWidget(index, 1, row.field);
            row.material = new QComboBox(table);
            row.material->addItem("None");
            for (const auto& material : materials) {
                row.material->addItem(material.first, q(material.second.material_id));
                row.material->setItemData(row.material->count() - 1, q(material.second.catalog_id), Qt::UserRole + 1);
            }
            row.material->setCurrentIndex(row.material_index);
            row.material->setAccessibleName(QString("Material of layer %1").arg(index + 1));
            table->setCellWidget(index, 2, row.material);
            QObject::connect(row.field, &QLineEdit::textChanged, owner, [this] { if (!rebuilding) (void)validate(); });
            QObject::connect(row.material, &QComboBox::currentIndexChanged, owner, [this] { if (!rebuilding) (void)validate(); });
        }
        table->resizeRowsToContents();
        if (selected >= 0 && !rows.empty()) table->selectRow(std::min(selected, static_cast<int>(rows.size()) - 1));
        rebuilding = false;
    }
    void move(int offset) {
        const int selected = table->currentRow(), target = selected + offset;
        if (selected < 0 || target < 0 || target >= static_cast<int>(rows.size())) return;
        capture(); std::swap(rows[selected], rows[target]); rebuild(target); (void)validate();
    }
    void fence() {
        require(!stale, "The project or drawing context changed. Reopen these layers.");
        try {
            const auto current = current_source();
            require(current.document_id() == source.document_id() && current.revision() == source.revision() &&
                current.saved_revision_optional() == source.saved_revision_optional() && current.dirty() == source.dirty() &&
                current.is_editable() == source.is_editable() && current.read_only_reason() == source.read_only_reason(),
                "The project changed. Reopen these layers.");
            require(source.shares_full_snapshot_with(current) || document_snapshot_digest(current) == digest,
                    "The project source changed. Reopen these layers.");
        } catch (...) { stale = true; throw; }
    }
    void fail(const QString& message) {
        accepted_intent.reset(); accepted_entity.reset(); error->setText(message); error->show(); save->setEnabled(false);
    }
    bool validate(std::optional<SlabLayerStackEditIntent>* intent_out = nullptr, std::optional<Entity>* entity_out = nullptr) {
        sum->setText("Layer sum: unavailable");
        try {
            require(source_error.isEmpty(), source_error.toStdString().c_str());
            fence();
            SlabLayerStackEditIntent intent; intent.slab_id = slab_id;
            if (thickness->text() != initial_thickness) intent.thickness = parse_quantity(thickness->text().toStdString(), unit);
            double total = 0;
            for (const auto& row : rows) {
                SlabLayerStackRow edit; edit.layer_id = row.id;
                if (row.added || row.field->text() != row.initial)
                    edit.thickness = parse_quantity(row.field->text().toStdString(), unit);
                total += edit.thickness ? edit.thickness->metres : row.native_thickness;
                const int selected = row.material->currentIndex();
                std::optional<WallLayerMaterial> material;
                if (selected > 0) material = materials.at(static_cast<std::size_t>(selected - 1)).second;
                if (row.added || material != row.original_material) {
                    edit.material_mode = material ? SlabLayerMaterialEditMode::set : SlabLayerMaterialEditMode::clear;
                    edit.material = std::move(material);
                }
                intent.layers.push_back(std::move(edit));
            }
            require(std::isfinite(total), "The layer sum is outside the supported range");
            const auto inches = total / 0.0254;
            sum->setText(unit == Unit::foot && std::isfinite(inches)
                ? QString("Layer sum: %1 in").arg(QString::number(inches, 'g', 6))
                : QString("Layer sum: %1 m").arg(QString::number(total, 'g', 8)));
            const auto replayed = replay_slab_layer_stack_entities(source.entities(), {intent});
            const auto candidate = replayed.at(slab_id);
            fence();
            if (intent_out) *intent_out = candidate == original ? std::nullopt : std::optional<SlabLayerStackEditIntent>(intent);
            if (entity_out) *entity_out = candidate;
            error->clear(); error->hide(); save->setEnabled(true); return true;
        } catch (const std::exception& reason) { fail(QString::fromUtf8(reason.what())); return false; }
        catch (...) { fail("The layers could not be validated. Reopen these layers."); return false; }
    }
};
HorizontalLayerStackDialog::HorizontalLayerStackDialog(const DocumentSnapshot& source, std::string id, bool metric,
    std::function<DocumentSnapshot()> provider, std::function<std::string()> allocator, QWidget* parent)
    : QDialog(parent), impl_(std::make_unique<Impl>(this, source, std::move(id), metric, std::move(provider), std::move(allocator))) {}
HorizontalLayerStackDialog::~HorizontalLayerStackDialog() = default;
std::optional<SlabLayerStackEditIntent> HorizontalLayerStackDialog::acceptedIntent() const { return impl_->accepted_intent; }
std::optional<Entity> HorizontalLayerStackDialog::acceptedEntity() const { return impl_->accepted_entity; }
QString HorizontalLayerStackDialog::lastError() const { return impl_->error->text(); }
bool HorizontalLayerStackDialog::submit() {
    if (!impl_->validate(&impl_->accepted_intent, &impl_->accepted_entity)) return false;
    QDialog::accept(); return true;
}
void HorizontalLayerStackDialog::accept() { (void)submit(); }
} // namespace sketch::desktop
