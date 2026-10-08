#include "horizontal_profile_dialog.hpp"

#include "sketch/assembly_model.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/project_organization.hpp"

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
#include <vector>

namespace sketch::desktop {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
QString q(const std::string& text) { return QString::fromStdString(text); }

QString label(const Entity& entity, const QString& fallback) {
    const auto name = entity.properties.find("name");
    if (name != entity.properties.end() && name->is_string()) {
        const auto text = q(name->get<std::string>()).trimmed();
        if (!text.isEmpty()) return text;
    }
    return fallback;
}

// Display native values without creating authored quantity authority. Untouched
// fields retain the exact actual source; only changed fields enter the parser.
QString native_metres(double value) {
    require(std::isfinite(value), "Profile dimensions must be finite");
    std::array<char, 768> buffer{};
    const auto displayed = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                          std::chars_format::general);
    require(displayed.ec == std::errc{}, "The native dimension cannot be displayed");
    return q(std::string(buffer.data(), displayed.ptr) + " m");
}

struct Input {
    QLineEdit* field{};
    QString initial_text;
    std::optional<Quantity> source_receipt;
};

std::optional<Quantity> receipt(const Entity& source,
                               const std::vector<std::string>& pointers, double metres) {
    const auto entries = source.properties.find("quantity_entries");
    if (entries == source.properties.end()) return std::nullopt;
    for (const auto& pointer : pointers) {
        const auto found = entries->find(pointer);
        if (found == entries->end() || !found->is_object() ||
            !found->contains("version") || found->at("version") != 1) continue;
        auto result = decode_constraint_quantity_receipt(*found);
        require(result.metres == metres, "The saved profile measurement is stale");
        return result;
    }
    return std::nullopt;
}

Input input(QWidget* parent, const char* object_name, double metres,
            const Entity& source, const std::vector<std::string>& pointers, Unit unit) {
    Input result;
    result.source_receipt = receipt(source, pointers, metres);
    if (result.source_receipt) {
        const auto& saved = *result.source_receipt;
        result.initial_text = q(saved.original_expression);
        // Bare original input may have been authored in different drawing units.
        // Keep the genuine receipt, but make the visible unit unambiguous.
        if (saved.entered_unit != unit) result.initial_text = q(format_quantity(saved, saved.entered_unit));
    } else {
        result.initial_text = native_metres(metres);
    }
    result.field = new QLineEdit(result.initial_text, parent);
    result.field->setObjectName(object_name);
    result.field->setMaxLength(4096);
    result.field->setToolTip(unit == Unit::metre
        ? "Enter metres, mm, cm, or feet and inches. A bare number uses metres."
        : "Enter feet and inches, or m, mm, cm. A bare number uses feet.");
    return result;
}

Quantity read(const Input& input, Unit unit) {
    if (input.field->text() == input.initial_text && input.source_receipt)
        return *input.source_receipt;
    return parse_quantity(input.field->text().toStdString(), unit);
}

QString material_name(const DocumentSnapshot& source, const SlabLayer& layer) {
    if (!layer.material) return "Unassigned";
    const auto& reference = *layer.material;
    const auto found = source.entities().find(reference.catalog_id);
    require(found != source.entities().end() && found->second.type == "assembly_model",
            "A layer material catalog is unavailable");
    const auto model = AssemblyModel::from_json(found->second.properties.at("model"));
    const auto material = std::find_if(model.materials().begin(), model.materials().end(),
        [&](const auto& item) { return item.id == reference.material_id; });
    require(material != model.materials().end(), "A layer material is unavailable in its catalog");
    return QString("%1 · %2").arg(q(material->name), label(found->second, q(reference.catalog_id)));
}

QTableWidgetItem* readonly_item(const QString& text) {
    auto* result = new QTableWidgetItem(text);
    result->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return result;
}
} // namespace

class HorizontalProfileDialog::Impl final {
public:
    HorizontalProfileDialog* owner;
    DocumentSnapshot source;
    std::string slab_id;
    std::string digest;
    Unit unit;
    std::function<DocumentSnapshot()> current_source;
    Entity original;
    Slab slab;
    Input thickness;
    Input elevation;
    std::vector<Input> layers;
    QLabel* sum{};
    QLabel* error{};
    QPushButton* save{};
    QString source_error;
    bool stale{};
    std::optional<SlabProfileEditIntent> accepted_intent;
    std::optional<Entity> accepted_entity;

    Impl(HorizontalProfileDialog* dialog, const DocumentSnapshot& captured,
         std::string id, bool metric, std::function<DocumentSnapshot()> provider)
        : owner(dialog), source(captured), slab_id(std::move(id)),
          digest(document_snapshot_digest(source)), unit(metric ? Unit::metre : Unit::foot),
          current_source(std::move(provider)) {
        owner->setObjectName("horizontalProfileDialog");
        owner->setWindowTitle("Horizontal profile");
        owner->resize(620, 380);
        auto* layout = new QVBoxLayout(owner);
        layout->setSpacing(12);
        auto* heading = new QLabel(owner);
        heading->setObjectName("horizontalProfileName");
        heading->setWordWrap(true);
        layout->addWidget(heading);
        auto* form = new QFormLayout;
        auto* total_row = new QHBoxLayout;
        sum = new QLabel(owner);
        sum->setObjectName("horizontalProfileLayerSum");
        sum->setAccessibleName("Total layer thickness");
        auto* table = new QTableWidget(0, 3, owner);
        table->setObjectName("horizontalProfileLayers");
        table->setAccessibleName("Existing layers, ordered from bottom to top");
        table->setHorizontalHeaderLabels({"Layer / ID", "Thickness", "Material / catalog"});
        table->verticalHeader()->hide();
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setAlternatingRowColors(true);
        table->setSortingEnabled(false);
        try {
            require(source.is_editable(), "The captured project is read only");
            require(static_cast<bool>(current_source), "The current project source is unavailable");
            const auto found = source.entities().find(slab_id);
            require(found != source.entities().end() && found->second.id == slab_id,
                    "The horizontal system is missing from the captured project");
            original = found->second;
            validate_slab_profile_source_entity(original);
            std::string diagnostic;
            if (!read_document_slab(original, slab, diagnostic))
                throw std::invalid_argument(diagnostic);
            heading->setText(label(original, "Horizontal system"));
            thickness = input(owner, "horizontalProfileThickness", slab.thickness, original,
                              {"/thickness_m", "/thickness"}, unit);
            thickness.field->setAccessibleName("Thickness");
            elevation = input(owner, "horizontalProfileElevation", slab.elevation, original,
                              {"/elevation_m", "/elevation"}, unit);
            elevation.field->setAccessibleName("Base elevation");
            total_row->addWidget(thickness.field, 1);
            total_row->addWidget(sum);
            form->addRow("Thickness", total_row);
            form->addRow("Base elevation", elevation.field);
            table->setRowCount(static_cast<int>(slab.layers.size()));
            layers.reserve(slab.layers.size());
            for (std::size_t i = 0; i < slab.layers.size(); ++i) {
                const auto& layer = slab.layers[i];
                const auto row = static_cast<int>(i);
                table->setItem(row, 0, readonly_item(QString("Layer %1").arg(row + 1)));
                table->item(row, 0)->setToolTip(q(layer.id));
                auto field = input(table, "horizontalProfileLayerThickness", layer.thickness, original,
                    {"/layers/" + std::to_string(i) + "/thickness_m"}, unit);
                field.field->setAccessibleName(QString("Thickness of layer %1").arg(row + 1));
                table->setCellWidget(row, 1, field.field);
                layers.push_back(std::move(field));
                table->setItem(row, 2, readonly_item(material_name(source, layer)));
            }
            table->resizeRowsToContents();
        } catch (const std::exception& reason) {
            source_error = QString::fromUtf8(reason.what());
            heading->setText("Horizontal system");
            table->setEnabled(false);
        }
        layout->addLayout(form);
        if (!slab.layers.empty()) {
            auto* order = new QLabel("Layers · bottom to top", owner);
            layout->addWidget(order);
            layout->addWidget(table, 1);
        } else {
            // A monolithic profile has no layer inventory to present.
            table->hide();
            sum->hide();
            owner->resize(560, 210);
        }
        error = new QLabel(owner);
        error->setObjectName("horizontalProfileError");
        error->setAccessibleName("Profile validation error");
        error->setWordWrap(true);
        layout->addWidget(error);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, owner);
        save = buttons->button(QDialogButtonBox::Save);
        save->setObjectName("saveHorizontalProfile");
        save->setAccessibleName("Save horizontal profile");
        save->setDefault(true);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, owner, [this] { (void)owner->submit(); });
        QObject::connect(buttons, &QDialogButtonBox::rejected, owner, &QDialog::reject);
        const auto watch = [this](QLineEdit* field) {
            if (field) QObject::connect(field, &QLineEdit::textChanged, owner, [this] { (void)validate(); });
        };
        watch(thickness.field);
        watch(elevation.field);
        for (const auto& field : layers) watch(field.field);
        auto* timer = new QTimer(owner);
        timer->setInterval(250);
        QObject::connect(timer, &QTimer::timeout, owner, [this] {
            if (stale || !source_error.isEmpty()) return;
            try { fence(); }
            catch (const std::exception& reason) { fail(QString::fromUtf8(reason.what())); }
            catch (...) { fail("The current project source is unavailable. Reopen this profile."); }
        });
        QObject::connect(owner, &QDialog::finished, timer, [this, timer](int result) {
            timer->stop();
            if (result != QDialog::Accepted) {
                accepted_intent.reset();
                accepted_entity.reset();
            }
        });
        timer->start();
        (void)validate();
    }

    void fence() {
        require(!stale, "The project or drawing context changed. Reopen this profile.");
        try {
            const auto current = current_source();
            require(current.document_id() == source.document_id() &&
                    current.revision() == source.revision() &&
                    current.saved_revision_optional() == source.saved_revision_optional() &&
                    current.dirty() == source.dirty() && current.is_editable() == source.is_editable() &&
                    current.read_only_reason() == source.read_only_reason(),
                    "The project changed. Reopen this profile.");
            require(source.shares_full_snapshot_with(current) || document_snapshot_digest(current) == digest,
                    "The project source changed. Reopen this profile.");
        } catch (...) {
            stale = true;
            throw;
        }
    }

    void fail(const QString& text) {
        accepted_intent.reset();
        accepted_entity.reset();
        error->setText(text);
        error->show();
        save->setEnabled(false);
    }

    bool validate(std::optional<SlabProfileEditIntent>* intent_out = nullptr,
                  std::optional<Entity>* entity_out = nullptr) {
        try {
            if (!source_error.isEmpty()) throw std::invalid_argument(source_error.toStdString());
            fence();
            SlabProfileEditIntent intent;
            intent.slab_id = slab_id;
            // An untouched scalar does not acquire new authored authority.
            if (thickness.field->text() != thickness.initial_text) intent.thickness = read(thickness, unit);
            if (elevation.field->text() != elevation.initial_text) intent.elevation = read(elevation, unit);
            const bool layers_edited = std::any_of(layers.begin(), layers.end(),
                [](const auto& field) { return field.field->text() != field.initial_text; });
            double total = 0;
            if (!layers.empty()) sum->setText("Layer sum: unavailable");
            if (layers_edited) intent.layer_thicknesses.emplace();
            for (std::size_t i = 0; i < layers.size(); ++i) {
                if (!layers_edited) { total += slab.layers[i].thickness; continue; }
                if (layers[i].field->text() == layers[i].initial_text) {
                    total += slab.layers[i].thickness;
                    intent.layer_thicknesses->push_back({slab.layers[i].id, Quantity{}, true});
                    continue;
                }
                const auto value = read(layers[i], unit);
                total += value.metres;
                intent.layer_thicknesses->push_back({slab.layers[i].id, value, false});
            }
            if (!layers.empty()) {
                require(std::isfinite(total), "The layer thickness sum is outside the supported range");
                // Read-only display only; this number never feeds authored input.
                if (unit == Unit::foot) {
                    const auto inches = total / 0.0254;
                    require(std::isfinite(inches), "The layer sum is outside the supported range");
                    sum->setText(QString("Layer sum: %1 in").arg(QString::number(inches, 'g', 6)));
                } else sum->setText(QString("Layer sum: %1 m").arg(QString::number(total, 'g', 8)));
            }
            Entity candidate = original;
            const bool has_input = intent.thickness || intent.elevation || intent.layer_thicknesses;
            if (has_input) {
                candidate = replay_slab_profile_entity(original, intent);
                const auto replayed = replay_slab_profile_entities(source.entities(), {intent});
                require(replayed.at(slab_id) == candidate, "The profile replay differs from its local source");
            } else {
                // A true no-op still admits actual resolved native placement.
                Slab resolved;
                std::string diagnostic;
                if (!read_document_slab(resolve_vertical_placement(source, original), resolved, diagnostic))
                    throw std::invalid_argument(diagnostic);
                (void)make_slab(resolved);
            }
            auto captured = has_input ? capture_slab_profile_edit(original, candidate, intent) : std::nullopt;
            // Catch provider/context changes that occurred during native replay.
            fence();
            if (intent_out) *intent_out = std::move(captured);
            if (entity_out) *entity_out = std::move(candidate);
            error->clear();
            error->hide();
            save->setEnabled(true);
            return true;
        } catch (const std::exception& reason) {
            if (!layers.empty() && sum->text().isEmpty()) sum->setText("Layer sum: unavailable");
            fail(QString::fromUtf8(reason.what()));
            return false;
        } catch (...) {
            fail("The profile could not be validated. Reopen this profile.");
            return false;
        }
    }
};

HorizontalProfileDialog::HorizontalProfileDialog(const DocumentSnapshot& source, std::string slab_id,
        bool metric, std::function<DocumentSnapshot()> current_source, QWidget* parent)
    : QDialog(parent), impl_(std::make_unique<Impl>(this, source, std::move(slab_id), metric,
                                                   std::move(current_source))) {}
HorizontalProfileDialog::~HorizontalProfileDialog() = default;
std::optional<SlabProfileEditIntent> HorizontalProfileDialog::acceptedIntent() const { return impl_->accepted_intent; }
std::optional<Entity> HorizontalProfileDialog::acceptedEntity() const { return impl_->accepted_entity; }
QString HorizontalProfileDialog::lastError() const { return impl_->error->text(); }
bool HorizontalProfileDialog::submit() {
    if (!impl_->validate(&impl_->accepted_intent, &impl_->accepted_entity)) return false;
    QDialog::accept();
    return true;
}
void HorizontalProfileDialog::accept() { (void)submit(); }

} // namespace sketch::desktop
