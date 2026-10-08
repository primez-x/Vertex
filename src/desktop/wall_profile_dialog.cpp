#include "wall_profile_dialog.hpp"

#include "sketch/assembly_model.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/phase_wall_profile_capture.hpp"
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
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sketch::desktop {
namespace {
using Entities = std::map<std::string, Entity, std::less<>>;

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

// Display only: an unreceipted native double is never converted to an authored
// Quantity. Untouched fields retain their actual source even outside the exact
// rational parser's range. Only edited text enters the parser.
QString native_text(double metres) {
    require(std::isfinite(metres), "Profile dimensions must be finite");
    std::array<char, 768> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), metres,
                                      std::chars_format::general);
    require(result.ec == std::errc{}, "The native dimension cannot be displayed");
    return q(std::string(buffer.data(), result.ptr) + " m");
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
    require(entries->is_object(), "The saved profile measurements are invalid");
    for (const auto& pointer : pointers) {
        const auto found = entries->find(pointer);
        if (found == entries->end()) continue;
        auto value = decode_constraint_quantity_receipt(*found);
        require(value.metres == metres, "The saved profile measurement is stale");
        return value;
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
        bool same_units = false;
        try { same_units = parse_quantity(saved.original_expression, unit).metres == metres; }
        catch (const std::exception&) {}
        // Retain the original receipt; this exact rendering disambiguates bare
        // saved input authored in a different drawing unit.
        if (!same_units) result.initial_text = q(format_quantity(saved, saved.entered_unit));
    } else {
        result.initial_text = native_text(metres);
    }
    result.field = new QLineEdit(result.initial_text, parent);
    result.field->setObjectName(object_name);
    result.field->setMaxLength(4096);
    result.field->setToolTip(unit == Unit::metre
        ? "Enter metres, mm, cm, or feet and inches. A bare number uses metres."
        : "Enter feet and inches, or m, mm, cm. A bare number uses feet.");
    return result;
}

bool edited(const Input& value) { return value.field->text() != value.initial_text; }
Quantity read(const Input& value, Unit unit) {
    require(edited(value), "An untouched profile measurement must retain its actual source");
    return parse_quantity(value.field->text().toStdString(), unit);
}

QString material_name(const Entities& source, const WallLayerMaterial& reference) {
    const auto found = source.find(reference.catalog_id);
    require(found != source.end() && found->second.id == reference.catalog_id &&
            found->second.type == "assembly_model", "A material catalog is unavailable");
    const auto model = AssemblyModel::from_json(found->second.properties.at("model"));
    const auto material = std::find_if(model.materials().begin(), model.materials().end(),
        [&](const auto& item) { return item.id == reference.material_id; });
    require(material != model.materials().end(), "A material is unavailable in its catalog");
    return QString("%1 · %2").arg(q(material->name), label(found->second, q(reference.catalog_id)));
}

// The physical replay resolves placement and admits native cuts/assemblies and
// joins. Also admit actual catalog references and drawing contexts on this
// same saved-active dependency graph; none of these are editable here.
void admit_context_materials(const Entities& entities, const std::string& wall_id) {
    const auto scope = constraint_phase_scope(entities);
    std::set<std::string, std::less<>> walls{wall_id};
    for (const auto& [id, entity] : entities) {
        if (entity.type != "wall_join" || scope.inactive_owner_ids.contains(id)) continue;
        const auto join = parse_wall_join(entity.properties, id);
        if (std::any_of(join.wall_ids.begin(), join.wall_ids.end(), [&](const auto& member) {
                return scope.inactive_owner_ids.contains(member);
            }) || std::find(join.wall_ids.begin(), join.wall_ids.end(), wall_id) == join.wall_ids.end()) continue;
        walls.insert(join.wall_ids.begin(), join.wall_ids.end());
    }
    const auto organization = organize_project(entities);
    for (const auto& [id, entity] : entities) {
        if (scope.inactive_owner_ids.contains(id)) continue;
        bool affected = entity.type == "wall" && walls.contains(id);
        if (entity.type == "opening") {
            const auto host = entity.properties.find("wall_id");
            affected = host != entity.properties.end() && host->is_string() &&
                walls.contains(host->get_ref<const std::string&>());
        }
        if (!affected) continue;
        require(entity.id == id, "A wall dependency has inconsistent identity");
        const bool scoped = entity.properties.contains("property_id") || entity.properties.contains("building_id") ||
            entity.properties.contains("floor_id") || entity.properties.contains("layer_id") ||
            entity.properties.contains("level_id") || entity.properties.contains("wall_id");
        const auto node = organization.nodes.find(id);
        require(!scoped || (node != organization.nodes.end() && node->second.issues.empty()),
                "A wall dependency has unresolved drawing context");
        if (const auto assignment = entity.properties.find("material_assignment");
            assignment != entity.properties.end()) {
            require(assignment->is_object() && assignment->contains("version") &&
                    assignment->at("version").is_number_integer() && assignment->at("version") == 1 &&
                    assignment->contains("catalog_id") && assignment->at("catalog_id").is_string() &&
                    assignment->contains("material_id") && assignment->at("material_id").is_string(),
                    "A wall dependency material assignment is unsupported");
            (void)material_name(entities, {assignment->at("catalog_id").get<std::string>(),
                                          assignment->at("material_id").get<std::string>()});
        }
        if (entity.type == "wall") {
            Wall wall;
            std::string diagnostic;
            if (!read_document_wall(entity, {}, wall, diagnostic)) throw std::invalid_argument(diagnostic);
            for (const auto& layer : wall.layers)
                if (layer.material) (void)material_name(entities, *layer.material);
        }
    }
}

bool bound_top_plane(const Entity& source) {
    if (!source.properties.contains("top_plane")) return false;
    const auto entries = source.properties.find("quantity_entries");
    if (entries == source.properties.end()) return false;
    for (auto entry = entries->begin(); entry != entries->end(); ++entry)
        if (entry.key() == "/top_plane" || entry.key().starts_with("/top_plane/")) return true;
    return false;
}

QTableWidgetItem* readonly_item(const QString& text) {
    auto* result = new QTableWidgetItem(text);
    result->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return result;
}
} // namespace

class WallProfileDialog::Impl final {
public:
    WallProfileDialog* owner;
    DocumentSnapshot source;
    std::string wall_id;
    std::string digest;
    Unit unit;
    std::function<DocumentSnapshot()> current_source;
    Entity original;
    Wall wall;
    Input thickness;
    Input height;
    Input rise;
    std::vector<Input> layers;
    QLabel* sum{};
    QLabel* error{};
    QPushButton* save{};
    QString source_error;
    bool stale{};
    std::optional<WallProfileEditIntent> accepted_intent;
    std::optional<Entity> accepted_entity;

    Impl(WallProfileDialog* dialog, const DocumentSnapshot& captured, std::string id,
         bool metric, std::function<DocumentSnapshot()> provider)
        : owner(dialog), source(captured), wall_id(std::move(id)),
          digest(document_snapshot_digest(source)), unit(metric ? Unit::metre : Unit::foot),
          current_source(std::move(provider)) {
        owner->setObjectName("wallProfileDialog");
        owner->setWindowTitle("Wall profile");
        owner->resize(640, 440);
        auto* layout = new QVBoxLayout(owner);
        layout->setSpacing(12);
        auto* heading = new QLabel("Wall", owner);
        heading->setObjectName("wallProfileName");
        heading->setWordWrap(true);
        layout->addWidget(heading);
        auto* form = new QFormLayout;
        auto* total_row = new QHBoxLayout;
        sum = new QLabel(owner);
        sum->setObjectName("wallProfileLayerSum");
        sum->setAccessibleName("Total layer thickness");
        auto* table = new QTableWidget(0, 3, owner);
        table->setObjectName("wallProfileLayers");
        table->setAccessibleName("Existing layers ordered from negative to positive baseline normal");
        table->setHorizontalHeaderLabels({"Layer", "Thickness", "Material"});
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
            const auto found = source.entities().find(wall_id);
            require(found != source.entities().end() && found->second.id == wall_id,
                    "The wall is missing from the captured project");
            original = found->second;
            validate_wall_profile_source_entity(original);
            std::string diagnostic;
            if (!read_document_wall(original, {}, wall, diagnostic)) throw std::invalid_argument(diagnostic);
            heading->setText(label(original, "Wall"));
            thickness = input(owner, "wallProfileThickness", wall.thickness, original,
                              {"/thickness_m", "/thickness"}, unit);
            thickness.field->setAccessibleName("Total wall thickness");
            height = input(owner, "wallProfileHeight", wall.height, original, {"/height_m", "/height"}, unit);
            height.field->setAccessibleName("Wall height at baseline start");
            rise = input(owner, "wallProfileTopRise", wall.slope_rise.value_or(0.0), original,
                         {"/slope_rise_m", "/slope_rise"}, unit);
            rise.field->setAccessibleName("Signed wall top rise from baseline start to end");
            rise.field->setToolTip(rise.field->toolTip() +
                " Signed rise follows the baseline from start to end. Zero clears a supported slope plane.");
            if (bound_top_plane(original)) {
                rise.field->setReadOnly(true);
                rise.field->setToolTip("This top plane has retained measurement bindings. Its rise cannot be changed here.");
            }
            // Elevation is retained by the typed profile intent. Display the
            // actual resolved base, including a captured level placement.
            Wall resolved;
            if (!read_document_wall(resolve_vertical_placement(source, original), {}, resolved, diagnostic))
                throw std::invalid_argument(diagnostic);
            auto* elevation = new QLineEdit(native_text(resolved.elevation), owner);
            elevation->setObjectName("wallProfileElevation");
            elevation->setAccessibleName("Resolved wall base elevation, read only");
            elevation->setReadOnly(true);
            elevation->setToolTip("Base elevation is retained by this profile edit, including level placement.");
            total_row->addWidget(thickness.field, 1);
            total_row->addWidget(sum);
            form->addRow("Total thickness", total_row);
            form->addRow("Height at start", height.field);
            form->addRow("Base elevation", elevation);
            form->addRow("Top rise", rise.field);
            table->setRowCount(static_cast<int>(wall.layers.size()));
            layers.reserve(wall.layers.size());
            for (std::size_t i = 0; i < wall.layers.size(); ++i) {
                const auto& layer = wall.layers[i];
                const auto row = static_cast<int>(i);
                table->setItem(row, 0, readonly_item(QString("Layer %1").arg(row + 1)));
                table->item(row, 0)->setToolTip(q(layer.id));
                auto field = input(table, "wallProfileLayerThickness", layer.thickness, original,
                                   {"/layers/" + std::to_string(i) + "/thickness_m"}, unit);
                field.field->setAccessibleName(QString("Thickness of layer %1").arg(q(layer.id)));
                table->setCellWidget(row, 1, field.field);
                layers.push_back(std::move(field));
                table->setItem(row, 2, readonly_item(layer.material ? material_name(source.entities(), *layer.material) : "Unassigned"));
            }
            table->resizeRowsToContents();
        } catch (const std::exception& reason) {
            source_error = QString::fromUtf8(reason.what());
            table->setEnabled(false);
        }
        layout->addLayout(form);
        if (!wall.layers.empty()) {
            auto* order = new QLabel("Layers · negative to positive baseline normal", owner);
            order->setWordWrap(true);
            layout->addWidget(order);
            layout->addWidget(table, 1);
        } else {
            table->hide();
            sum->hide();
            owner->resize(560, 290);
        }
        error = new QLabel(owner);
        error->setObjectName("wallProfileError");
        error->setAccessibleName("Wall profile validation error");
        error->setWordWrap(true);
        layout->addWidget(error);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, owner);
        save = buttons->button(QDialogButtonBox::Save);
        save->setObjectName("saveWallProfile");
        save->setAccessibleName("Save wall profile");
        save->setDefault(true);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, owner, [this] { (void)owner->submit(); });
        QObject::connect(buttons, &QDialogButtonBox::rejected, owner, &QDialog::reject);
        const auto watch = [this](QLineEdit* field) {
            if (field) QObject::connect(field, &QLineEdit::textChanged, owner, [this] { (void)validate(); });
        };
        watch(thickness.field);
        watch(height.field);
        watch(rise.field);
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
            if (result != QDialog::Accepted) { accepted_intent.reset(); accepted_entity.reset(); }
        });
        timer->start();
        (void)validate();
    }

    void fence() {
        require(!stale, "The project or drawing context changed. Reopen this profile.");
        try {
            const auto current = current_source();
            require(current.document_id() == source.document_id() && current.revision() == source.revision() &&
                    current.saved_revision_optional() == source.saved_revision_optional() &&
                    current.dirty() == source.dirty() && current.is_editable() == source.is_editable() &&
                    current.read_only_reason() == source.read_only_reason(),
                    "The project changed. Reopen this profile.");
            require(source.shares_full_snapshot_with(current) || document_snapshot_digest(current) == digest,
                    "The project source changed. Reopen this profile.");
        } catch (...) { stale = true; throw; }
    }

    void fail(const QString& text) {
        accepted_intent.reset();
        accepted_entity.reset();
        error->setText(text);
        error->show();
        save->setEnabled(false);
    }

    bool validate(std::optional<WallProfileEditIntent>* intent_out = nullptr,
                  std::optional<Entity>* entity_out = nullptr) {
        try {
            if (!source_error.isEmpty()) throw std::invalid_argument(source_error.toStdString());
            fence();
            WallProfileEditIntent intent;
            intent.wall_id = wall_id;
            if (edited(thickness)) intent.thickness = read(thickness, unit);
            if (edited(height)) intent.height = read(height, unit);
            if (edited(rise)) {
                require(!rise.field->isReadOnly(), "This retained top plane cannot be changed here");
                intent.top_rise = read(rise, unit);
            }
            const bool layers_edited = std::any_of(layers.begin(), layers.end(), [](const auto& field) { return edited(field); });
            if (!layers.empty()) sum->setText("Layer sum: unavailable");
            double total = 0;
            std::vector<std::optional<Quantity>> quantities(layers.size());
            for (std::size_t i = 0; i < layers.size(); ++i) {
                if (edited(layers[i])) {
                    quantities[i] = read(layers[i], unit);
                    total += quantities[i]->metres;
                } else {
                    // Display only; the intent retains this actual source
                    // row without inventing a quantity from its native value.
                    total += wall.layers[i].thickness;
                }
            }
            if (!layers.empty()) {
                require(std::isfinite(total), "The layer thickness sum is outside the supported range");
                // Derived display only. Total thickness always remains an
                // explicit independent input; no implicit redistribution.
                sum->setText(unit == Unit::metre
                    ? QString("Layer sum: %1 m").arg(QString::number(total, 'g', 8))
                    : QString("Layer sum: %1 in").arg(QString::number(total / 0.0254, 'g', 6)));
            }
            if (layers_edited) {
                intent.layer_thicknesses.emplace();
                for (std::size_t i = 0; i < layers.size(); ++i) {
                    if (quantities[i]) intent.layer_thicknesses->push_back({wall.layers[i].id, *quantities[i], false});
                    else intent.layer_thicknesses->push_back({wall.layers[i].id, Quantity{}, true});
                }
            }
            admit_context_materials(source.entities(), wall_id);
            Entity candidate = original;
            const bool has_input = intent.thickness || intent.height || intent.top_rise || intent.layer_thicknesses;
            if (has_input) {
                candidate = replay_wall_profile_entity(original, intent);
                // Final room/relationship consequences belong to the
                // controller's atomic ordinary/proposed publication. Admit
                // the actual native graph here without requiring an old room
                // lock to accept a temporary profile stage.
                const auto replayed = replay_wall_profile_entities(source.entities(), {intent}, false, true);
                require(replayed.at(wall_id) == candidate, "The profile replay differs from its local source");
                admit_context_materials(replayed, wall_id);
                // Replay returns exact source early for equivalent inputs;
                // still admit current residuals before accepting a no-op.
                const auto& residual_source = candidate == original &&
                    candidate.properties.dump() == original.properties.dump() &&
                    candidate.extensions.dump() == original.extensions.dump() ? replayed : source.entities();
                if (const auto diagnostic = validate_active_phase_constraint_integrity(residual_source))
                    throw std::invalid_argument(*diagnostic);
            } else {
                validate_active_wall_physical_dependencies(source.entities(), {wall_id}, true);
                if (const auto diagnostic = validate_active_phase_constraint_integrity(source.entities()))
                    throw std::invalid_argument(*diagnostic);
            }
            auto captured = has_input ? capture_wall_profile_edit(original, candidate, intent) : std::nullopt;
            fence();
            if (intent_out) *intent_out = std::move(captured);
            if (entity_out) *entity_out = std::move(candidate);
            error->clear();
            error->hide();
            save->setEnabled(true);
            return true;
        } catch (const std::exception& reason) {
            fail(QString::fromUtf8(reason.what()));
            return false;
        } catch (...) {
            fail("The wall profile could not be validated. Reopen this profile.");
            return false;
        }
    }
};

WallProfileDialog::WallProfileDialog(const DocumentSnapshot& source, std::string wall_id,
        bool metric, std::function<DocumentSnapshot()> current_source, QWidget* parent)
    : QDialog(parent), impl_(std::make_unique<Impl>(this, source, std::move(wall_id), metric,
                                                  std::move(current_source))) {}
WallProfileDialog::~WallProfileDialog() = default;
std::optional<WallProfileEditIntent> WallProfileDialog::acceptedIntent() const { return impl_->accepted_intent; }
std::optional<Entity> WallProfileDialog::acceptedEntity() const { return impl_->accepted_entity; }
QString WallProfileDialog::lastError() const { return impl_->error->text(); }
bool WallProfileDialog::submit() {
    if (!impl_->validate(&impl_->accepted_intent, &impl_->accepted_entity)) return false;
    QDialog::accept();
    return true;
}
void WallProfileDialog::accept() { (void)submit(); }

} // namespace sketch::desktop
