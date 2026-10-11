#include "sketch/desktop/selection_properties_dialog.hpp"

#include "sketch/annotation_entity_codec.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/phase_wall_profile_capture.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/roof_entity_codec.hpp"
#include "sketch/structural_object_edit.hpp"
#include "sketch/stair_profile_edit.hpp"
#include "sketch/stair_semantics.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStringList>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace sketch::desktop {
namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::invalid_argument("Selection Properties: " + message);
}
QString q(const std::string& value) { return QString::fromStdString(value); }
QString name(const Entity& entity, const QString& fallback) {
    const auto found = entity.properties.find("name");
    if (found != entity.properties.end() && found->is_string()) {
        const auto result = q(found->get<std::string>()).trimmed();
        if (!result.isEmpty()) return result;
    }
    return fallback;
}
std::string native(double value) {
    require(std::isfinite(value), "A saved dimension is not finite.");
    std::array<char, 768> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    require(converted.ec == std::errc{}, "A saved dimension cannot be displayed.");
    return {buffer.data(), converted.ptr};
}
double scalar(const Entity& entity, const char* canonical, const char* alias = nullptr) {
    const auto found = entity.properties.find(canonical);
    const auto legacy = alias ? entity.properties.find(alias) : entity.properties.end();
    const auto selected = found != entity.properties.end() ? found : legacy;
    require(selected != entity.properties.end() && selected->is_number(),
            "A saved profile dimension is missing on " + entity.id + ".");
    const auto result = selected->get<double>();
    require(std::isfinite(result), "A saved profile dimension is not finite on " + entity.id + ".");
    require(legacy == entity.properties.end() || (legacy->is_number() && legacy->get<double>() == result),
            "Saved profile aliases disagree on " + entity.id + ".");
    return result;
}

// Exact retained numeric coordinates do not acquire authority from optional
// metadata. Reference-image v1 edits historically preserve raw receipt maps.
Quantity exact_numeric_quantity(const Entity& source, double metres) {
    require(std::isfinite(metres), "A saved dimension is not finite on " + source.id + ".");
    std::array<char, 768> buffer{};
    for (int precision = 0; precision <= 18; ++precision) {
        const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), metres,
                                             std::chars_format::fixed, precision);
        if (converted.ec != std::errc{}) continue;
        try {
            auto result = parse_quantity(std::string(buffer.data(), converted.ptr) + " m", Unit::metre);
            if (result.metres == metres) return result;
        } catch (const std::exception&) {}
    }
    throw std::invalid_argument("Selection Properties unavailable for " + source.id +
        ": an unchanged profile dimension has no exact supported measurement receipt.");
}

// Same-value intent bodies are required by the ordinary codec. Prefer a real
// receipt; numeric fallback must parse back to the exact actual double.
Quantity invariant_quantity(const Entity& source, double metres,
                            std::initializer_list<const char*> pointers) {
    if (const auto entries = source.properties.find("quantity_entries"); entries != source.properties.end()) {
        require(entries->is_object(), "Saved measurement receipts are invalid on " + source.id + ".");
        for (const auto* pointer : pointers) {
            const auto found = entries->find(pointer);
            if (found == entries->end()) continue;
            require(found->is_object(), "A saved measurement receipt is invalid on " + source.id + ".");
            if (found->contains("version") && found->at("version") != 1) continue;
            auto result = decode_constraint_quantity_receipt(*found);
            require(result.metres == metres, "A saved measurement receipt is stale on " + source.id + ".");
            return result;
        }
    }
    return exact_numeric_quantity(source, metres);
}

std::optional<std::string> text(QLineEdit* field) {
    if (!field || field->text().trimmed().isEmpty()) return std::nullopt;
    return field->text().toStdString();
}
std::optional<Quantity> length(QLineEdit* field, Unit unit, bool zero = false, bool signed_value = false) {
    const auto entered = text(field);
    if (!entered) return std::nullopt;
    auto result = parse_quantity(*entered, unit);
    require(std::isfinite(result.metres) && (signed_value || (zero ? result.metres >= 0 : result.metres > 0)),
            field->accessibleName().toStdString() + (zero ? " must be nonnegative." : " must be positive."));
    return result;
}
std::optional<double> number(QLineEdit* field, double minimum, double maximum) {
    if (!text(field)) return std::nullopt;
    bool ok = false;
    const auto result = field->text().trimmed().toDouble(&ok);
    require(ok && std::isfinite(result) && result >= minimum && result <= maximum,
            field->accessibleName().toStdString() + " is outside the supported range.");
    return result;
}
std::optional<double> angle(QLineEdit* field) {
    const auto degrees = number(field, -180, 180);
    return degrees ? std::optional<double>{*degrees * (std::numbers::pi / 180.0)} : std::nullopt;
}
std::optional<std::string> color(QLineEdit* field) {
    auto result = text(field);
    if (!result) return result;
    *result = field->text().trimmed().toStdString();
    require(result->size() == 7 && result->front() == '#' &&
            result->find_first_not_of("0123456789abcdefABCDEF", 1) == std::string::npos,
            field->accessibleName().toStdString() + " must use #RRGGBB.");
    return result;
}
std::optional<bool> visibility(QComboBox* field) {
    return !field || field->currentIndex() == 0 ? std::nullopt :
        std::optional<bool>{field->currentIndex() == 1};
}
std::optional<std::string> annotation_layer(QComboBox* field) {
    return !field || !field->isEnabled() || field->currentIndex() == 0 ? std::nullopt :
        std::optional<std::string>{field->currentData().toString().toStdString()};
}
bool same_annotation_floor(const DrawingContext& a, const DrawingContext& b) {
    return std::tie(a.property_id, a.building_id, a.floor_id, a.level_id) ==
           std::tie(b.property_id, b.building_id, b.floor_id, b.level_id);
}
struct Fields {
    QLineEdit *width{}, *depth{}, *height{}, *thickness{}, *elevation{}, *offset{}, *sill{},
        *rise{}, *overhang{}, *rotation{}, *outline{}, *surface{}, *content{}, *scale{},
        *intensity{}, *frame{}, *curb{}, *glazing{}, *font{}, *x{}, *y{}, *radius{},
        *going{}, *landing_depth{}, *landing_thickness{}, *length{}, *post_spacing{};
    QComboBox *visible{}, *layer{}, *bold{}, *italic{}, *alignment{}, *flip_horizontal{}, *flip_vertical{};
};
struct LayerInput {
    std::string id;
    QLineEdit* field{};
};
struct OrdinarySource {
    SelectionPropertyTarget target;
    std::vector<LayerInput> layers;
};
struct PresentationSource {
    SelectionPresentationPropertyTarget target;
    std::optional<SymbolInstance> symbol;
    std::optional<SymbolDefinition> definition;
    std::optional<LabelInstance> label;
};
} // namespace

class SelectionPropertiesDialog::Impl final {
public:
    SelectionPropertiesDialog* owner;
    const DocumentSnapshot source;
    Unit unit;
    std::vector<OrdinarySource> ordinary;
    std::vector<PresentationSource> presentation;
    std::vector<RoofOpeningGroupMember> skylights;
    std::map<SelectionPropertyFamily, Fields> profiles;
    std::map<SelectionPresentationPropertyFamily, Fields> appearances;
    std::map<QLineEdit*, QString> current_values;
    std::map<QComboBox*, std::optional<bool>> current_visibility_values;
    std::map<QComboBox*, QString> current_choice_values;
    Fields skylight_fields;
    QTabWidget* tabs{};
    QTreeWidget* selection{};
    QLabel* error{};
    std::optional<SelectionPropertyEditRequest> accepted_ordinary;
    std::optional<SelectionPresentationPropertyEditRequest> accepted_presentation;
    std::optional<RoofOpeningGroupProperties> accepted_skylights;

    const Entity& entity(const std::string& id, const char* type) const {
        const auto found = source.entities().find(id);
        require(found != source.entities().end() && found->second.id == id && found->second.type == type,
                "Selected " + std::string(type) + " is missing or inconsistent: " + id + ".");
        return found->second;
    }
    QFormLayout* page(const QString& title) {
        auto* content = new QWidget(tabs);
        auto* form = new QFormLayout(content);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setVerticalSpacing(10);
        auto* scroll = new QScrollArea(tabs);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(content);
        tabs->addTab(scroll, title);
        return form;
    }
    QLineEdit* field(QFormLayout* form, const char* label, const char* object_name,
                     const QString& placeholder = "Unchanged") {
        auto* result = new QLineEdit(owner);
        result->setObjectName(object_name);
        result->setAccessibleName(label);
        result->setMaxLength(4096);
        result->setPlaceholderText(placeholder);
        result->setClearButtonEnabled(true);
        result->setToolTip("Leave blank to keep each selected item's current value.");
        form->addRow(label, result);
        return result;
    }
    QLineEdit* dimension(QFormLayout* form, const char* label, const char* object_name) {
        auto* result = field(form, label, object_name);
        result->setToolTip(unit == Unit::metre ?
            "Leave blank to keep current values. Enter m, mm, cm, or feet and inches; bare numbers use metres." :
            "Leave blank to keep current values. Enter feet and inches, or m, mm, cm; bare numbers use feet.");
        return result;
    }
    QComboBox* visible(QFormLayout* form, const char* object_name) {
        auto* result = new QComboBox(owner);
        result->setObjectName(object_name);
        result->setAccessibleName("Visibility");
        result->addItems({"Unchanged", "Visible", "Hidden"});
        form->addRow("Visibility", result);
        return result;
    }
    void current(QLineEdit* field, const QString& value) {
        auto [found, inserted] = current_values.emplace(field, value);
        if (!inserted && found->second != value) found->second = "Multiple values";
        field->setPlaceholderText(found->second + " (unchanged)");
    }
    void current_length(QLineEdit* field, double metres) {
        current(field, q(native(unit == Unit::metre ? metres : metres / 0.3048)) +
                      (unit == Unit::metre ? " m" : " ft"));
    }
    void current_angle(QLineEdit* field, double radians) {
        current(field, q(native(radians * (180.0 / std::numbers::pi))) + "°");
    }
    void current_visibility(QComboBox* field, bool value) {
        auto [found, inserted] = current_visibility_values.emplace(field, value);
        if (!inserted && found->second != std::optional<bool>{value}) found->second.reset();
        field->setItemText(0, !found->second ? "Unchanged (mixed)" :
            *found->second ? "Unchanged (visible)" : "Unchanged (hidden)");
    }
    QComboBox* choice(QFormLayout* form, const char* label, const char* object_name,
                      const QStringList& values) {
        auto* result = new QComboBox(owner);
        result->setObjectName(object_name);
        result->setAccessibleName(label);
        result->addItem("Unchanged");
        result->addItems(values);
        result->setToolTip("Unchanged keeps each selected item's current value.");
        form->addRow(label, result);
        return result;
    }
    void current_choice(QComboBox* field, const QString& value) {
        auto [found, inserted] = current_choice_values.emplace(field, value);
        if (!inserted && found->second != value) found->second = "mixed";
        field->setItemText(0, "Unchanged (" + found->second + ")");
    }
    void row(const QString& label, const QString& family, const QString& identity) {
        auto* item = new QTreeWidgetItem(selection, {label, family});
        item->setToolTip(0, identity);
        item->setToolTip(1, identity);
    }
    void annotation_layer_field(SelectionPresentationPropertyFamily family, QFormLayout* form,
                                const ProjectOrganization& organization) {
        auto* input = new QComboBox(owner);
        input->setObjectName(family == SelectionPresentationPropertyFamily::symbol ?
            "selectionSymbolLayer" : "selectionLabelLayer");
        input->setAccessibleName(family == SelectionPresentationPropertyFamily::symbol ?
            "Symbol layer" : "Text layer");
        appearances.at(family).layer = input;
        form->addRow("Layer", input);
        const auto layer_context = [&](const std::string& id) -> std::optional<DrawingContext> {
            const auto found = organization.nodes.find(id);
            return found == organization.nodes.end() || found->second.type != "layer" ?
                std::nullopt : organization.drawing_context(id);
        };
        const auto layer_name = [&](const std::string& id) -> QString {
            if (id.empty()) return "No explicit layer";
            const auto context = layer_context(id);
            if (!context) return "Unresolved layer (" + q(id) + ")";
            return q(organization.nodes.at(context->floor_id).name) + " / " +
                   q(organization.nodes.at(id).name);
        };
        std::set<std::string> current_layers;
        std::optional<DrawingContext> common_context;
        QString unavailable;
        for (const auto& value : presentation) {
            if (value.target.family != family) continue;
            const auto& placement = value.symbol ? value.symbol->placement : value.label->placement;
            current_layers.insert(placement.layer_id);
            if (!unavailable.isEmpty()) continue;
            const auto& original = source.entities().at(value.target.owner_id);
            const bool scoped = original.properties.contains("property_id") ||
                original.properties.contains("building_id") || original.properties.contains("floor_id") ||
                original.properties.contains("layer_id") || original.properties.contains("level_id");
            auto context = organization.drawing_context(original.id);
            // Derive choices only. Original-source preparation remains the final
            // authority, including validation of retained members and identity edits.
            if (scoped && (!context || (original.properties.contains("level_id") &&
                original.properties.at("level_id") != context->level_id))) {
                unavailable = "Layer assignment unavailable: a selected annotation's declared owner context is unresolved or contradictory.";
                continue;
            }
            const auto child_context = layer_context(placement.layer_id);
            if (!placement.layer_id.empty() && (!child_context ||
                (context && !same_annotation_floor(*context, *child_context)))) {
                unavailable = "Layer assignment unavailable: a selected annotation's current layer is unresolved or contradicts its owner floor.";
                continue;
            }
            // Never infer a floor from the requested destination, or fall back
            // from a declared but invalid owner to its child's layer.
            if (!context && !scoped) context = child_context;
            if (!context) {
                unavailable = "Layer assignment unavailable: a selected annotation has no resolved original floor and layer context.";
            } else if (common_context && !same_annotation_floor(*common_context, *context)) {
                unavailable = "Layer assignment unavailable: the selected annotations do not share one property, building, floor and level. Edit each floor separately.";
            } else {
                common_context = context;
            }
        }
        input->addItem(current_layers.size() == 1 ?
            "Unchanged (" + layer_name(*current_layers.begin()) + ")" : "Unchanged (mixed layers)");
        QString current_assignments;
        for (const auto& id : current_layers) {
            if (!current_assignments.isEmpty()) current_assignments += "\n";
            current_assignments += layer_name(id) + (id.empty() ? QString{} : " [" + q(id) + "]");
        }
        input->setItemData(0, current_assignments, Qt::ToolTipRole);
        if (unavailable.isEmpty() && common_context) {
            std::vector<std::pair<QString, std::string>> choices;
            for (const auto& [id, node] : organization.nodes) {
                if (node.type != "layer") continue;
                const auto context = organization.drawing_context(id);
                if (context && same_annotation_floor(*common_context, *context))
                    choices.emplace_back(layer_name(id), id);
            }
            std::sort(choices.begin(), choices.end());
            for (const auto& [label, id] : choices) {
                input->addItem(label, q(id));
                input->setItemData(input->count() - 1, q(id), Qt::ToolTipRole);
            }
        }
        if (unavailable.isEmpty() && input->count() == 1)
            unavailable = "Layer assignment unavailable: no resolved layer is valid for every selected annotation.";
        input->setCurrentIndex(0);
        const auto explanation = unavailable.isEmpty() ?
            QString("Choose a layer for every selected item on %1. Unchanged keeps each current assignment.")
                .arg(q(organization.nodes.at(common_context->floor_id).name)) : unavailable;
        input->setEnabled(unavailable.isEmpty());
        input->setToolTip(explanation + "\nCurrent assignments:\n" + current_assignments);
        input->setAccessibleDescription(input->itemText(0) + ". " + explanation);
        if (!unavailable.isEmpty()) {
            auto* note = new QLabel(explanation, owner);
            note->setObjectName(family == SelectionPresentationPropertyFamily::symbol ?
                "selectionSymbolLayerContext" : "selectionLabelLayerContext");
            note->setWordWrap(true);
            form->addRow(note);
        }
    }
    template<class Layers>
    void layer_fields(OrdinarySource& target, const Layers& layers, QFormLayout* form, const Entity& original) {
        if (layers.empty()) return;
        auto* heading = new QLabel(name(original, "Selected system") + " — layers", owner);
        heading->setWordWrap(true);
        form->addRow(heading);
        for (std::size_t index = 0; index < layers.size(); ++index) {
            const auto& layer = layers[index];
            const auto label = QString("Layer %1 thickness").arg(static_cast<qulonglong>(index + 1));
            auto* input = dimension(form, label.toUtf8().constData(), "selectionLayerThickness");
            input->setPlaceholderText(q(native(layer.thickness) + " m (unchanged)"));
            input->setToolTip(q(original.id + " / " + layer.id) +
                "\nLeave blank to retain this layer and its saved measurement. Total and layer thicknesses must agree.");
            target.layers.push_back({layer.id, input});
        }
    }

    Impl(SelectionPropertiesDialog* dialog, const DocumentSnapshot& captured,
         std::vector<SelectionPresentationPropertyTarget> presentation_targets,
         std::vector<SelectionPropertyTarget> ordinary_targets,
         std::vector<RoofOpeningGroupMember> members, bool metric)
        : owner(dialog), source(captured), unit(metric ? Unit::metre : Unit::foot), skylights(std::move(members)) {
        require(source.is_editable(), "The captured project is read only.");
        const auto count = presentation_targets.size() + ordinary_targets.size() + skylights.size();
        require(count > 0 && count <= 1000, "Select between one and 1000 supported items.");
        owner->setObjectName("selectionPropertiesDialog");
        owner->setWindowTitle("Selection properties");
        owner->resize(600, 570);
        auto* layout = new QVBoxLayout(owner);
        layout->setSpacing(10);
        auto* heading = new QLabel(QString("%1 selected items").arg(static_cast<qulonglong>(count)), owner);
        layout->addWidget(heading);
        selection = new QTreeWidget(owner);
        selection->setObjectName("selectionPropertiesRoster");
        selection->setAccessibleName("Complete selection");
        selection->setColumnCount(2);
        selection->setHeaderLabels({"Name", "Kind"});
        selection->setRootIsDecorated(false);
        selection->setAlternatingRowColors(true);
        selection->setEditTriggers(QAbstractItemView::NoEditTriggers);
        selection->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        selection->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        selection->setMaximumHeight(150);
        layout->addWidget(selection);
        auto* instruction = new QLabel("Enter values to apply to the matching selected items. Blank fields keep current values.", owner);
        instruction->setWordWrap(true);
        layout->addWidget(instruction);
        tabs = new QTabWidget(owner);
        tabs->setObjectName("selectionPropertiesFamilies");
        layout->addWidget(tabs, 1);

        std::sort(ordinary_targets.begin(), ordinary_targets.end(), [](const auto& a, const auto& b) { return a.entity_id < b.entity_id; });
        std::set<std::string> ordinary_ids;
        std::map<SelectionPropertyFamily, QFormLayout*> profile_forms;
        for (const auto& target : ordinary_targets) {
            require(ordinary_ids.insert(target.entity_id).second, "An ordinary selected owner occurs twice.");
            const char* type = nullptr;
            const char* title = nullptr;
            switch (target.family) {
            case SelectionPropertyFamily::wall: type = "wall"; title = "Walls"; break;
            case SelectionPropertyFamily::slab: type = "slab"; title = "Slabs"; break;
            case SelectionPropertyFamily::hosted_opening: type = "opening"; title = "Doors and windows"; break;
            case SelectionPropertyFamily::roof: type = "roof"; title = "Roofs"; break;
            case SelectionPropertyFamily::column: type = "column"; title = "Columns"; break;
            case SelectionPropertyFamily::beam: type = "beam"; title = "Beams"; break;
            case SelectionPropertyFamily::stair: type = "stair"; title = "Stairs"; break;
            case SelectionPropertyFamily::railing: type = "railing"; title = "Railings"; break;
            default: throw std::invalid_argument("Selection Properties unavailable: unsupported ordinary family.");
            }
            const auto& original = entity(target.entity_id, type);
            if (!profile_forms.contains(target.family)) {
                auto* form = page(title);
                profile_forms.emplace(target.family, form);
                auto& f = profiles[target.family];
                if (target.family == SelectionPropertyFamily::wall) {
                    f.thickness = dimension(form, "Total thickness", "selectionWallThickness");
                    f.height = dimension(form, "Height", "selectionWallHeight");
                    f.rise = dimension(form, "Top rise / fall", "selectionWallTopRise");
                    f.rise->setToolTip("Signed change in top height from wall start to end. Zero clears the slope; leave blank to retain the current top profile.");
                } else if (target.family == SelectionPropertyFamily::slab) {
                    f.thickness = dimension(form, "Total thickness", "selectionSlabThickness");
                    f.elevation = dimension(form, "Base elevation", "selectionSlabElevation");
                } else if (target.family == SelectionPropertyFamily::hosted_opening) {
                    f.width = dimension(form, "Width", "selectionOpeningWidth");
                    f.height = dimension(form, "Height", "selectionOpeningHeight");
                    f.sill = dimension(form, "Sill height", "selectionOpeningSill");
                    f.offset = dimension(form, "Offset along wall", "selectionOpeningOffset");
                } else if (target.family == SelectionPropertyFamily::column) {
                    f.height = dimension(form, "Height (all columns)", "selectionColumnHeight");
                } else if (target.family == SelectionPropertyFamily::beam) {
                    f.width = dimension(form, "Width", "selectionBeamWidth");
                    f.depth = dimension(form, "Depth", "selectionBeamDepth");
                } else if (target.family == SelectionPropertyFamily::stair) {
                    f.rise = dimension(form, "Total rise", "selectionStairTotalRise");
                    f.going = dimension(form, "Going", "selectionStairGoing");
                    f.width = dimension(form, "Width", "selectionStairWidth");
                    auto* note = new QLabel("Going and width changes apply to the stair defaults and all existing flight overrides. Blank fields retain each flight's current dimensions. Top landing fields apply only to stairs that already have a top landing.", owner);
                    note->setWordWrap(true);
                    form->addRow(note);
                } else if (target.family == SelectionPropertyFamily::railing) {
                    f.height = dimension(form, "Height", "selectionRailingHeight");
                    f.thickness = dimension(form, "Thickness", "selectionRailingThickness");
                    f.post_spacing = dimension(form, "Post spacing", "selectionRailingPostSpacing");
                    auto* note = new QLabel("Length applies only to independent railings. Attached railing length follows its stair and cannot be entered here. Blank fields retain current values.", owner);
                    note->setWordWrap(true);
                    form->addRow(note);
                } else if (target.family == SelectionPropertyFamily::roof) {
                    f.width = dimension(form, "Length / horizontal run", "selectionRoofLength");
                    f.depth = dimension(form, "Span", "selectionRoofSpan");
                    f.rise = dimension(form, "Rise", "selectionRoofRise");
                    f.overhang = dimension(form, "Overhang", "selectionRoofOverhang");
                    f.thickness = dimension(form, "Panel thickness", "selectionRoofThickness");
                }
            }
            OrdinarySource value{target, {}};
            const auto* title_singular = type;
            if (target.family == SelectionPropertyFamily::wall) {
                validate_wall_profile_source_entity(original);
                Wall wall;
                std::string diagnostic;
                require(read_document_wall(original, {}, wall, diagnostic), target.entity_id + ": " + diagnostic);
                layer_fields(value, wall.layers, profile_forms.at(target.family), original);
                current_length(profiles.at(target.family).thickness, wall.thickness);
                current_length(profiles.at(target.family).height, wall.height);
                current_length(profiles.at(target.family).rise, wall.slope_rise.value_or(0.0));
                title_singular = "Wall";
            } else if (target.family == SelectionPropertyFamily::slab) {
                validate_slab_profile_source_entity(original);
                Slab slab;
                std::string diagnostic;
                require(read_document_slab(original, slab, diagnostic), target.entity_id + ": " + diagnostic);
                layer_fields(value, slab.layers, profile_forms.at(target.family), original);
                current_length(profiles.at(target.family).thickness, slab.thickness);
                current_length(profiles.at(target.family).elevation, slab.elevation);
                title_singular = "Slab";
            } else if (target.family == SelectionPropertyFamily::hosted_opening) {
                validate_hosted_opening_profile_entity(original);
                const auto& f = profiles.at(target.family);
                current_length(f.width, scalar(original, "width_m", "width"));
                current_length(f.height, scalar(original, "height_m", "height"));
                current_length(f.sill, scalar(original, "sill_m", "sill"));
                current_length(f.offset, scalar(original, "offset_m", "offset"));
                title_singular = "Door / window";
            } else if (target.family == SelectionPropertyFamily::column || target.family == SelectionPropertyFamily::beam) {
                validate_structural_object_source_entity(original);
                auto& f = profiles.at(target.family);
                const auto& structural_form = original.properties.at("form");
                if (target.family == SelectionPropertyFamily::beam) {
                    require(structural_form == "straight_beam", "Selected beam form is unsupported.");
                    current_length(f.width, scalar(original, "width_m"));
                    current_length(f.depth, scalar(original, "depth_m"));
                    title_singular = "Beam";
                } else {
                    current_length(f.height, scalar(original, "height_m"));
                    auto* form = profile_forms.at(target.family);
                    if (structural_form == "rectangular_column") {
                        if (!f.width) {
                            f.width = dimension(form, "Rectangular width", "selectionColumnWidth");
                            f.depth = dimension(form, "Rectangular depth", "selectionColumnDepth");
                        }
                        current_length(f.width, scalar(original, "width_m"));
                        current_length(f.depth, scalar(original, "depth_m"));
                    } else {
                        require(structural_form == "circular_column", "Selected column form is unsupported.");
                        if (!f.radius) f.radius = dimension(form, "Circular radius", "selectionColumnRadius");
                        current_length(f.radius, scalar(original, "radius_m"));
                    }
                    title_singular = "Column";
                }
            } else if (target.family == SelectionPropertyFamily::stair) {
                validate_stair_profile_source_entity(original);
                const auto stair = decode_stair_properties(original.id, original.properties);
                auto& f = profiles.at(target.family);
                current_length(f.rise, stair.total_rise);
                current_length(f.going, stair.going);
                current_length(f.width, stair.width);
                for (const auto& flight : stair.flights) {
                    current_length(f.going, flight.going.value_or(stair.going));
                    current_length(f.width, flight.width.value_or(stair.width));
                }
                if (stair.top_landing) {
                    if (!f.landing_depth) {
                        auto* form = profile_forms.at(target.family);
                        f.landing_depth = dimension(form, "Existing top landing depth", "selectionStairTopLandingDepth");
                        f.landing_thickness = dimension(form, "Existing top landing thickness", "selectionStairTopLandingThickness");
                    }
                    current_length(f.landing_depth, stair.top_landing->depth);
                    current_length(f.landing_thickness, stair.top_landing->thickness);
                }
                title_singular = "Stair";
            } else if (target.family == SelectionPropertyFamily::railing) {
                validate_stair_profile_source_entity(original);
                const auto rail = decode_railing_properties(original.id, original.properties);
                auto& f = profiles.at(target.family);
                current_length(f.height, rail.height);
                current_length(f.thickness, rail.thickness);
                current_length(f.post_spacing, rail.post_spacing);
                if (!rail.host && !rail.landing_host) {
                    if (!f.length) f.length = dimension(profile_forms.at(target.family), "Independent length", "selectionRailingLength");
                    current_length(f.length, rail.length);
                }
                title_singular = rail.host || rail.landing_host ? "Attached railing" : "Independent railing";
            } else if (target.family == SelectionPropertyFamily::roof) {
                (void)decode_roof_entity(original);
                const auto& f = profiles.at(target.family);
                current_length(f.width, scalar(original, original.properties.at("form") == "sloped_roof_panel" ? "run_m" : "length_m"));
                current_length(f.depth, scalar(original, "span_m"));
                current_length(f.rise, scalar(original, "rise_m"));
                current_length(f.overhang, scalar(original, "overhang_m"));
                current_length(f.thickness, scalar(original, "thickness_m"));
                title_singular = "Roof";
            }
            row(name(original, QString("%1 %2").arg(title_singular).arg(static_cast<qulonglong>(ordinary.size() + 1))),
                title_singular, q(target.entity_id));
            ordinary.push_back(std::move(value));
        }
        for (const auto& [family, form] : profile_forms) {
            if (family != SelectionPropertyFamily::wall && family != SelectionPropertyFamily::slab) continue;
            if (std::none_of(ordinary.begin(), ordinary.end(), [&](const auto& target) {
                    return target.target.family == family && !target.layers.empty(); })) continue;
            auto* note = new QLabel("Changing total thickness requires matching explicit layer thicknesses. Layers retain their current order and materials.", owner);
            note->setWordWrap(true);
            form->addRow(note);
        }

        std::sort(presentation_targets.begin(), presentation_targets.end(), [](const auto& a, const auto& b) {
            return std::tie(a.owner_id, a.child_id) < std::tie(b.owner_id, b.child_id);
        });
        std::set<std::pair<std::string, std::string>> presentation_ids;
        std::map<SelectionPresentationPropertyFamily, QFormLayout*> appearance_forms;
        for (const auto& target : presentation_targets) {
            require(!ordinary_ids.contains(target.owner_id), "A presentation target aliases an ordinary selected owner.");
            require(presentation_ids.emplace(target.owner_id, target.child_id).second,
                    "A qualified presentation member occurs twice.");
            const char* title = nullptr;
            switch (target.family) {
            case SelectionPresentationPropertyFamily::symbol: title = "Symbols"; break;
            case SelectionPresentationPropertyFamily::label: title = "Text"; break;
            case SelectionPresentationPropertyFamily::reference: title = "Reference images"; break;
            default: throw std::invalid_argument("Selection Properties unavailable: unsupported presentation family.");
            }
            if (!appearance_forms.contains(target.family)) {
                auto* form = page(title);
                appearance_forms.emplace(target.family, form);
                auto& f = appearances[target.family];
                if (target.family == SelectionPresentationPropertyFamily::symbol) {
                    f.x = dimension(form, "X position", "selectionSymbolX");
                    f.y = dimension(form, "Y position", "selectionSymbolY");
                    f.width = dimension(form, "Physical width", "selectionSymbolWidth");
                    f.depth = dimension(form, "Physical depth", "selectionSymbolDepth");
                    f.outline = field(form, "Outline color", "selectionSymbolOutline", "#111111 (unchanged)");
                    f.surface = field(form, "Surface color", "selectionSymbolSurface", "#FFFFFF (unchanged)");
                } else if (target.family == SelectionPresentationPropertyFamily::label) {
                    f.x = dimension(form, "X position", "selectionLabelX");
                    f.y = dimension(form, "Y position", "selectionLabelY");
                    f.content = field(form, "Text", "selectionLabelContent");
                    f.content->setMaxLength(65536);
                    f.height = dimension(form, "Text size", "selectionLabelSize");
                    f.outline = field(form, "Text color", "selectionLabelColor", "#RRGGBB (unchanged)");
                    f.font = field(form, "Font family", "selectionLabelFontFamily");
                    f.font->setMaxLength(256);
                    f.bold = choice(form, "Bold", "selectionLabelBold", {"On", "Off"});
                    f.italic = choice(form, "Italic", "selectionLabelItalic", {"On", "Off"});
                    f.alignment = choice(form, "Alignment", "selectionLabelAlignment", {"Left", "Center", "Right"});
                } else {
                    f.x = dimension(form, "X position", "selectionReferenceX");
                    f.y = dimension(form, "Y position", "selectionReferenceY");
                    f.scale = field(form, "Scale", "selectionReferenceScale");
                    f.scale->setToolTip("Absolute image scale relative to its calibrated source. Leave blank to retain each current scale.");
                    f.intensity = field(form, "Intensity (0–1)", "selectionReferenceIntensity");
                }
                for (auto* coordinate : {f.x, f.y})
                    coordinate->setToolTip("Position in each object's saved drawing coordinates. Leave blank to retain its current position.");
                if (target.family != SelectionPresentationPropertyFamily::label) {
                    f.flip_horizontal = choice(form, "Mirror horizontally", target.family == SelectionPresentationPropertyFamily::symbol ?
                        "selectionSymbolMirrorHorizontal" : "selectionReferenceMirrorHorizontal", {"On", "Off"});
                    f.flip_vertical = choice(form, "Mirror vertically", target.family == SelectionPresentationPropertyFamily::symbol ?
                        "selectionSymbolMirrorVertical" : "selectionReferenceMirrorVertical", {"On", "Off"});
                }
                f.rotation = field(form, "Rotation (degrees)", target.family == SelectionPresentationPropertyFamily::symbol ?
                    "selectionSymbolRotation" : target.family == SelectionPresentationPropertyFamily::label ?
                    "selectionLabelRotation" : "selectionReferenceRotation");
                f.rotation->setToolTip("Absolute angle from −180° to 180°. Leave blank to retain each current angle.");
                f.visible = visible(form, target.family == SelectionPresentationPropertyFamily::symbol ?
                    "selectionSymbolVisibility" : target.family == SelectionPresentationPropertyFamily::label ?
                    "selectionLabelVisibility" : "selectionReferenceVisibility");
            }
            PresentationSource value{target, {}, {}, {}};
            if (target.family == SelectionPresentationPropertyFamily::reference) {
                require(target.child_id.empty(), "A reference image cannot name an annotation child.");
                const auto& original = entity(target.owner_id, "reference_asset");
                const auto& f = appearances.at(target.family);
                current_length(f.x, original.properties.at("position_m").at(0).get<double>());
                current_length(f.y, original.properties.at("position_m").at(1).get<double>());
                current(f.scale, q(native(original.properties.value("scale", 1.0))));
                current(f.intensity, q(native(original.properties.value("intensity", 1.0))));
                current(f.rotation, q(native(original.properties.value("rotation_degrees", 0.0))) + "°");
                current_visibility(f.visible, original.properties.value("visible", true));
                current_choice(f.flip_horizontal, original.properties.value("flip_horizontal", false) ? "on" : "off");
                current_choice(f.flip_vertical, original.properties.value("flip_vertical", false) ? "on" : "off");
                row(name(original, QString("Reference image %1").arg(static_cast<qulonglong>(presentation.size() + 1))),
                    "Reference image", q(target.owner_id));
            } else {
                const auto& original = entity(target.owner_id, kAnnotationEntityType);
                const auto state = decode_annotation_entity(original);
                std::size_t matches = 0;
                for (const auto& symbol : state.symbols) if (symbol.id == target.child_id) {
                    ++matches;
                    require(target.family == SelectionPresentationPropertyFamily::symbol, "Selected text actually names a symbol.");
                    value.symbol = symbol;
                }
                for (const auto& label : state.labels) if (label.id == target.child_id) {
                    ++matches;
                    require(target.family == SelectionPresentationPropertyFamily::label, "Selected symbol actually names text.");
                    value.label = label;
                }
                require(matches == 1, "Selected annotation is missing or ambiguous: " + target.owner_id + " / " + target.child_id + ".");
                if (value.symbol) {
                    value.definition = resolved_symbol_definition(*value.symbol, default_symbol_catalog());
                    // Preserve saved SVG eligibility even when the renderer's
                    // resolver suppresses a stale external artwork path.
                    if (value.symbol->definition) value.definition = *value.symbol->definition;
                    const auto& f = appearances.at(target.family);
                    const auto& symbol = *value.symbol;
                    current_length(f.x, symbol.placement.position.x);
                    current_length(f.y, symbol.placement.position.y);
                    current_choice(f.flip_horizontal, symbol.flip_horizontal ? "on" : "off");
                    current_choice(f.flip_vertical, symbol.flip_vertical ? "on" : "off");
                    current_length(f.width, value.definition->width_metres * (symbol.placement.scale * symbol.width_scale));
                    current_length(f.depth, value.definition->depth_metres * (symbol.placement.scale * symbol.depth_scale));
                    current_angle(f.rotation, symbol.placement.rotation_radians);
                    current_visibility(f.visible, symbol.visible);
                    const auto palette = symbol.svg_palette.value_or(SymbolSvgPalette{});
                    current(f.outline, q(value.definition->svg_asset ? palette.outline_color : symbol.style.stroke_color));
                    current(f.surface, q(value.definition->svg_asset ? palette.surface_color : symbol.style.fill_color));
                    row(q(value.definition->name.empty() ? value.symbol->symbol_id : value.definition->name), "Symbol",
                        q(target.owner_id + " / " + target.child_id));
                } else {
                    const auto& f = appearances.at(target.family);
                    current_length(f.x, value.label->placement.position.x);
                    current_length(f.y, value.label->placement.position.y);
                    current(f.content, q(value.label->content));
                    current_length(f.height, value.label->style.text_height_metres);
                    current(f.outline, q(value.label->style.stroke_color));
                    current(f.font, q(value.label->style.font_family));
                    current_choice(f.bold, value.label->style.bold ? "on" : "off");
                    current_choice(f.italic, value.label->style.italic ? "on" : "off");
                    current_choice(f.alignment, q(value.label->style.text_alignment));
                    current_angle(f.rotation, value.label->placement.rotation_radians);
                    current_visibility(f.visible, value.label->visible);
                    row(q(value.label->content).isEmpty() ? "Empty text" : q(value.label->content), "Text",
                        q(target.owner_id + " / " + target.child_id));
                }
            }
            presentation.push_back(std::move(value));
        }
        if (appearance_forms.contains(SelectionPresentationPropertyFamily::symbol) ||
            appearance_forms.contains(SelectionPresentationPropertyFamily::label)) {
            const auto organization = organize_project(source);
            for (const auto& [family, form] : appearance_forms)
                if (family != SelectionPresentationPropertyFamily::reference)
                    annotation_layer_field(family, form, organization);
        }

        std::sort(skylights.begin(), skylights.end(), [](const auto& a, const auto& b) {
            return std::tie(a.roof_id, a.opening_id) < std::tie(b.roof_id, b.opening_id);
        });
        std::set<std::pair<std::string, std::string>> skylight_ids;
        if (!skylights.empty()) {
            auto* form = page("Skylights");
            auto& f = skylight_fields;
            f.width = dimension(form, "Face width", "selectionSkylightWidth");
            f.depth = dimension(form, "Face depth", "selectionSkylightDepth");
            f.rotation = field(form, "Rotation (degrees)", "selectionSkylightRotation");
            f.rotation->setToolTip("Absolute facet angle from −180° to 180°. Leave blank to retain current angles.");
            f.frame = dimension(form, "Frame width", "selectionSkylightFrame");
            f.curb = dimension(form, "Curb height", "selectionSkylightCurb");
            f.glazing = dimension(form, "Glazing thickness", "selectionSkylightGlazing");
        }
        for (const auto& member : skylights) {
            require(skylight_ids.emplace(member.roof_id, member.opening_id).second, "A selected skylight occurs twice.");
            const auto& original = entity(member.roof_id, "roof");
            const auto object = decode_roof_entity(original);
            std::visit([&](const auto& roof) {
                const auto found = std::find_if(roof.openings.begin(), roof.openings.end(), [&](const auto& child) {
                    return child.id == member.opening_id;
                });
                require(found != roof.openings.end() && found->skylight.has_value(),
                        "Selected roof child is unavailable: Properties currently requires a profiled skylight (" + member.opening_id + ").");
                const auto frame = roof_opening_plan_frame(roof, *found);
                const auto& f = skylight_fields;
                current_length(f.width, found->width * frame.width_surface_scale);
                current_length(f.depth, found->depth * frame.depth_surface_scale);
                current_angle(f.rotation, found->rotation_radians);
                current_length(f.frame, found->skylight->frame_width);
                current_length(f.curb, found->skylight->curb_height);
                current_length(f.glazing, found->skylight->glazing_thickness);
            }, object);
            row(name(original, "Roof") + QString(" / Skylight %1").arg(static_cast<qulonglong>(skylight_ids.size())),
                "Skylight", q(member.roof_id + " / " + member.opening_id));
        }
        error = new QLabel(owner);
        error->setObjectName("selectionPropertiesError");
        error->setWordWrap(true);
        error->hide();
        layout->addWidget(error);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, owner);
        auto* apply = buttons->addButton("Apply", QDialogButtonBox::AcceptRole);
        apply->setObjectName("applySelectionProperties");
        apply->setDefault(true);
        layout->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::rejected, owner, &QDialog::reject);
        QObject::connect(buttons, &QDialogButtonBox::accepted, owner, [this] { (void)owner->submit(); });
    }

    SelectionPropertyEditRequest ordinary_request() const {
        SelectionPropertyEditRequest request;
        request.unchanged_policy = SelectionPropertyUnchangedPolicy::retain;
        for (const auto& value : ordinary) {
            const auto& target = value.target;
            request.selected.push_back(target);
            const auto& original = source.entities().at(target.entity_id);
            const auto& f = profiles.at(target.family);
            if (target.family == SelectionPropertyFamily::wall) {
                WallProfileEditIntent intent;
                intent.wall_id = target.entity_id;
                intent.thickness = length(f.thickness, unit);
                intent.height = length(f.height, unit);
                intent.top_rise = length(f.rise, unit, false, true);
                if (std::any_of(value.layers.begin(), value.layers.end(), [](const auto& layer) { return text(layer.field).has_value(); })) {
                    intent.layer_thicknesses.emplace();
                    for (const auto& layer : value.layers) {
                        const auto entered = length(layer.field, unit);
                        intent.layer_thicknesses->push_back({layer.id, entered.value_or(Quantity{}), !entered.has_value()});
                    }
                }
                if (!intent.thickness && !intent.height && !intent.layer_thicknesses && !intent.top_rise)
                    intent.height = invariant_quantity(original, scalar(original, "height_m", "height"), {"/height_m", "/height"});
                (void)encode_wall_profile_edit_intent(intent);
                // Detached scalar/layer admission is safe. Hosted cuts, joined
                // owners and rooms belong to the enclosing complete compositor.
                (void)replay_wall_profile_entity(original, intent);
                request.walls.push_back(std::move(intent));
            } else if (target.family == SelectionPropertyFamily::slab) {
                SlabProfileEditIntent intent;
                intent.slab_id = target.entity_id;
                intent.thickness = length(f.thickness, unit);
                intent.elevation = length(f.elevation, unit, false, true);
                if (std::any_of(value.layers.begin(), value.layers.end(), [](const auto& layer) { return text(layer.field).has_value(); })) {
                    intent.layer_thicknesses.emplace();
                    for (const auto& layer : value.layers) {
                        const auto entered = length(layer.field, unit);
                        intent.layer_thicknesses->push_back({layer.id, entered.value_or(Quantity{}), !entered.has_value()});
                    }
                }
                if (!intent.thickness && !intent.elevation && !intent.layer_thicknesses)
                    intent.thickness = invariant_quantity(original, scalar(original, "thickness_m", "thickness"), {"/thickness_m", "/thickness"});
                (void)encode_slab_profile_edit_intent(intent);
                (void)replay_slab_profile_entity(original, intent);
                request.slabs.push_back(std::move(intent));
            } else if (target.family == SelectionPropertyFamily::hosted_opening) {
                HostedOpeningProfileEditIntent intent;
                intent.opening_id = target.entity_id;
                intent.wall_id = original.properties.at("wall_id").get<std::string>();
                intent.width = length(f.width, unit);
                intent.height = length(f.height, unit);
                intent.sill = length(f.sill, unit, true);
                intent.offset = length(f.offset, unit, true);
                if (!intent.width && !intent.height && !intent.sill && !intent.offset)
                    intent.width = invariant_quantity(original, scalar(original, "width_m", "width"), {"/width_m", "/width"});
                (void)encode_hosted_opening_profile_edit_intent(intent);
                (void)replay_hosted_opening_profile_entity(original, intent);
                request.hosted_openings.push_back(std::move(intent));
            } else if (target.family == SelectionPropertyFamily::column || target.family == SelectionPropertyFamily::beam) {
                validate_structural_object_source_entity(original);
                StructuralProfileEditIntent intent;
                intent.object_id = target.entity_id;
                if (target.family == SelectionPropertyFamily::beam || original.properties.at("form") == "rectangular_column") {
                    intent.width = length(f.width, unit);
                    intent.depth = length(f.depth, unit);
                } else {
                    intent.radius = length(f.radius, unit);
                }
                if (target.family == SelectionPropertyFamily::column) intent.height = length(f.height, unit);
                request.structural_profiles.push_back(std::move(intent));
            } else if (target.family == SelectionPropertyFamily::stair || target.family == SelectionPropertyFamily::railing) {
                validate_stair_profile_source_entity(original);
                StairProfileEditIntent intent;
                intent.object_id = target.entity_id;
                if (target.family == SelectionPropertyFamily::stair) {
                    intent.total_rise = length(f.rise, unit);
                    intent.going = length(f.going, unit);
                    intent.width = length(f.width, unit);
                    if (decode_stair_properties(original.id, original.properties).top_landing) {
                        intent.top_landing_depth = length(f.landing_depth, unit);
                        intent.top_landing_thickness = length(f.landing_thickness, unit);
                    }
                } else {
                    intent.height = length(f.height, unit);
                    intent.thickness = length(f.thickness, unit);
                    intent.post_spacing = length(f.post_spacing, unit);
                    const auto rail = decode_railing_properties(original.id, original.properties);
                    if (!rail.host && !rail.landing_host) intent.length = length(f.length, unit);
                }
                (void)encode_stair_profile_edit_intent(intent);
                request.stair_profiles.push_back(std::move(intent));
            } else if (target.family == SelectionPropertyFamily::roof) {
                RoofProfileEditIntent intent;
                intent.roof_id = target.entity_id;
                intent.length = length(f.width, unit);
                intent.span = length(f.depth, unit);
                intent.rise = length(f.rise, unit, true);
                intent.overhang = length(f.overhang, unit, true);
                intent.thickness = length(f.thickness, unit);
                if (!intent.length && !intent.span && !intent.rise && !intent.overhang && !intent.thickness)
                    intent.thickness = invariant_quantity(original, scalar(original, "thickness_m"), {"/thickness_m"});
                (void)encode_roof_profile_edit_intent(intent);
                // Source-bound scalar staging deliberately defers roof/child fit.
                (void)stage_roof_profile_entity(original, intent);
                request.roofs.push_back(std::move(intent));
            }
        }
        return request;
    }

    SelectionPresentationPropertyEditRequest presentation_request() const {
        SelectionPresentationPropertyEditRequest request;
        request.unchanged_policy = SelectionPresentationPropertyUnchangedPolicy::retain;
        for (const auto& value : presentation) {
            const auto& target = value.target;
            request.selected.push_back(target);
            const auto& f = appearances.at(target.family);
            if (target.family == SelectionPresentationPropertyFamily::reference) {
                SelectionReferencePropertyEditIntent intent;
                intent.entity_id = target.owner_id;
                const auto x = length(f.x, unit, false, true);
                const auto y = length(f.y, unit, false, true);
                if (x || y) {
                    const auto& original = source.entities().at(target.owner_id);
                    intent.position = std::array<Quantity, 2>{
                        x ? *x : exact_numeric_quantity(original, original.properties.at("position_m").at(0).get<double>()),
                        y ? *y : exact_numeric_quantity(original, original.properties.at("position_m").at(1).get<double>())};
                }
                intent.rotation_degrees = number(f.rotation, -180, 180);
                intent.scale = number(f.scale, std::numeric_limits<double>::min(), std::numeric_limits<double>::max());
                intent.intensity = number(f.intensity, 0, 1);
                intent.visible = visibility(f.visible);
                intent.flip_horizontal = visibility(f.flip_horizontal);
                intent.flip_vertical = visibility(f.flip_vertical);
                request.references.push_back(std::move(intent));
                continue;
            }
            SelectionAnnotationPresentationPropertyEdit patch;
            const auto x = length(f.x, unit, false, true);
            const auto y = length(f.y, unit, false, true);
            if (x || y) {
                const auto& original = source.entities().at(target.owner_id);
                const auto& position = value.symbol ? value.symbol->placement.position : value.label->placement.position;
                // Keep the saved source frame and each member's actual untouched
                // coordinate; optional raw receipts may refer to an older pose.
                patch.position = std::array<Quantity, 2>{
                    x ? *x : exact_numeric_quantity(original, position.x),
                    y ? *y : exact_numeric_quantity(original, position.y)};
            }
            patch.rotation_radians = angle(f.rotation);
            patch.visible = visibility(f.visible);
            patch.layer_id = annotation_layer(f.layer);
            const auto outline = color(f.outline);
            if (target.family == SelectionPresentationPropertyFamily::symbol) {
                SelectionSymbolPropertyEditIntent intent;
                intent.owner_id = target.owner_id;
                intent.child_id = target.child_id;
                intent.width = length(f.width, unit);
                intent.depth = length(f.depth, unit);
                intent.flip_horizontal = visibility(f.flip_horizontal);
                intent.flip_vertical = visibility(f.flip_vertical);
                const auto surface = color(f.surface);
                if (outline || surface) {
                    if (value.definition->svg_asset) {
                        auto palette = value.symbol->svg_palette.value_or(SymbolSvgPalette{});
                        if (outline) palette.outline_color = *outline;
                        if (surface) palette.surface_color = *surface;
                        intent.svg_palette = std::optional<SymbolSvgPalette>{std::move(palette)};
                    } else {
                        SelectionAnnotationStylePropertyEdit style;
                        style.stroke_color = outline;
                        style.fill_color = surface;
                        patch.style = std::move(style);
                    }
                }
                intent.presentation = std::move(patch);
                request.symbols.push_back(std::move(intent));
            } else {
                SelectionLabelPropertyEditIntent intent;
                intent.owner_id = target.owner_id;
                intent.child_id = target.child_id;
                intent.content = text(f.content);
                const auto height = length(f.height, unit);
                const auto font = text(f.font);
                const auto bold = visibility(f.bold);
                const auto italic = visibility(f.italic);
                const auto alignment = f.alignment->currentIndex() == 0 ? std::nullopt :
                    std::optional<std::string>{f.alignment->currentText().toLower().toStdString()};
                if (outline || height || font || bold || italic || alignment) {
                    SelectionAnnotationStylePropertyEdit style;
                    style.stroke_color = outline;
                    style.text_height = height;
                    style.font_family = font;
                    style.bold = bold;
                    style.italic = italic;
                    style.text_alignment = alignment;
                    patch.style = std::move(style);
                }
                intent.presentation = std::move(patch);
                request.labels.push_back(std::move(intent));
            }
        }
        (void)prepare_selection_presentation_property_edit(source, request);
        return request;
    }
    RoofOpeningGroupProperties skylight_request() const {
        RoofOpeningGroupProperties request;
        request.members = skylights;
        request.retain_invariant_members = true;
        const auto input = [&](QLineEdit* field, bool zero = false) -> std::optional<RoofOpeningQuantityInput> {
            const auto entered = length(field, unit, zero);
            return entered ? std::optional<RoofOpeningQuantityInput>{RoofOpeningQuantityInput{*entered, unit}} : std::nullopt;
        };
        const auto& f = skylight_fields;
        request.width = input(f.width);
        request.depth = input(f.depth);
        request.frame_width = input(f.frame);
        request.curb_height = input(f.curb, true);
        request.glazing_thickness = input(f.glazing);
        request.rotation_radians = angle(f.rotation);
        // The mixed producer must admit the final parent/child fit together.
        return request;
    }
};

SelectionPropertiesDialog::SelectionPropertiesDialog(const DocumentSnapshot& captured,
    std::vector<SelectionPresentationPropertyTarget> presentation_targets,
    std::vector<SelectionPropertyTarget> ordinary_targets,
    std::vector<RoofOpeningGroupMember> skylight_members, bool metric, QWidget* parent)
    : QDialog(parent), impl_(std::make_unique<Impl>(this, captured, std::move(presentation_targets),
          std::move(ordinary_targets), std::move(skylight_members), metric)) {}
SelectionPropertiesDialog::~SelectionPropertiesDialog() = default;
std::optional<SelectionPropertyEditRequest> SelectionPropertiesDialog::ordinaryRequest() const {
    return impl_->accepted_ordinary;
}
std::optional<SelectionPresentationPropertyEditRequest> SelectionPropertiesDialog::presentationRequest() const {
    return impl_->accepted_presentation;
}
std::optional<RoofOpeningGroupProperties> SelectionPropertiesDialog::roofOpeningProperties() const {
    return impl_->accepted_skylights;
}
QString SelectionPropertiesDialog::lastError() const { return impl_->error->text(); }
bool SelectionPropertiesDialog::submit() {
    impl_->accepted_ordinary.reset();
    impl_->accepted_presentation.reset();
    impl_->accepted_skylights.reset();
    try {
        const auto ordinary = impl_->ordinary.empty() ? std::nullopt :
            std::optional<SelectionPropertyEditRequest>{impl_->ordinary_request()};
        const auto presentation = impl_->presentation.empty() ? std::nullopt :
            std::optional<SelectionPresentationPropertyEditRequest>{impl_->presentation_request()};
        const auto skylights = impl_->skylights.empty() ? std::nullopt :
            std::optional<RoofOpeningGroupProperties>{impl_->skylight_request()};
        impl_->accepted_ordinary = ordinary;
        impl_->accepted_presentation = presentation;
        impl_->accepted_skylights = skylights;
        impl_->error->clear();
        impl_->error->hide();
        accept();
        return true;
    } catch (const std::exception& failure) {
        impl_->error->setText(QString::fromUtf8(failure.what()));
        impl_->error->show();
        return false;
    }
}

} // namespace sketch::desktop
