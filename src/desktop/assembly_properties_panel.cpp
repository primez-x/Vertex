#include "sketch/desktop/assembly_properties_panel.hpp"

#include "sketch/quantity.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStringList>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch::desktop {
namespace {
using Target = AssemblyInstancePropertyEditTarget;
using Intent = AssemblyInstancePropertyEditIntent;
using Operation = AssemblyInstancePropertyPatchOperation;
using Path = std::vector<std::string>;
constexpr std::size_t maximum_targets = 1000;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::invalid_argument("Assembly Properties: " + message);
}
QString q(const std::string& value) { return QString::fromStdString(value); }
QString exact(double value) { return QString::number(value, 'g', 17); }
QString unit_name(AssemblyQuantityUnit unit) {
    switch (unit) {
    case AssemblyQuantityUnit::count: return "count";
    case AssemblyQuantityUnit::metre: return "m";
    case AssemblyQuantityUnit::square_metre: return "m2";
    case AssemblyQuantityUnit::cubic_metre: return "m3";
    case AssemblyQuantityUnit::kilogram: return "kg";
    }
    throw std::invalid_argument("Assembly Properties: unknown declared quantity unit.");
}
QString path_label(const Path& path) {
    QStringList parts;
    for (const auto& id : path) parts.push_back(q(id));
    return parts.join(" / ");
}
bool prefix(const Path& parent, const Path& child) {
    return parent.size() <= child.size() && std::equal(parent.begin(), parent.end(), child.begin());
}
AssemblyTransform root_pose(const AssemblyInstance& instance) {
    if (instance.placement) {
        const auto& p = *instance.placement;
        return {{p.translation_m.x, p.translation_m.y, p.translation_z_m},
            p.rotation_radians, p.scale, p.mirrored_y, p.vertical_scale};
    }
    return instance.root_transform.value_or(AssemblyTransform{});
}
bool changed(const Intent& intent) {
    const auto any = [](const auto& values) {
        return std::any_of(values.begin(), values.end(), [](const auto& item) {
            return item.second.operation != Operation::unchanged;
        });
    };
    return any(intent.properties) || any(intent.materials) || any(intent.quantities) ||
        intent.pose.operation != Operation::unchanged;
}
double scalar(QLineEdit* field, bool positive = false) {
    bool ok = false;
    const auto value = field->text().trimmed().toDouble(&ok);
    require(ok && std::isfinite(value) && (!positive || value > 0),
        field->accessibleName().toStdString() + (positive ? " must be finite and positive." : " must be a finite number."));
    return value;
}
bool supplied(QLineEdit* field) { return !field->text().trimmed().isEmpty(); }

struct Source {
    Target target;
    const AssemblyModel* model{};
    const AssemblyExpandedNode* node{};
    const AssemblyType* type{};
    AssemblyTransform pose;
};
struct ValueRow {
    std::string key;
    QComboBox* operation{};
    QLineEdit* text{};
    QComboBox* material{};
    AssemblyQuantityUnit unit{AssemblyQuantityUnit::count};
};
struct Page {
    std::vector<Source> sources;
    QWidget* widget{};
    std::vector<ValueRow> properties, materials, quantities;
    QComboBox *pose_operation{}, *mirrored{};
    std::array<QLineEdit*, 6> pose{};
};
template<class T, class Map>
QString common_hint(const std::vector<Source>& sources, Map values, const std::string& key,
                    const auto& display) {
    const T& first = values(*sources.front().node).at(key);
    for (const auto& source : sources)
        if (values(*source.node).at(key) != first) return "Multiple values";
    return display(first);
}
template<class Map>
auto common_slots(const std::vector<Source>& sources, Map values, const auto& compatible) {
    auto result = values(*sources.front().type);
    for (const auto& source : sources) {
        const auto& declared = values(*source.type);
        std::erase_if(result, [&](const auto& item) {
            const auto found = declared.find(item.first);
            return found == declared.end() || !compatible(item.second, found->second);
        });
    }
    return result;
}
Operation operation(QComboBox* field) {
    return static_cast<Operation>(field->currentData().toInt());
}
} // namespace

class AssemblyPropertiesPanel::Impl final {
public:
    AssemblyPropertiesPanel* owner;
    const DocumentSnapshot source;
    const Unit length_unit;
    std::vector<Target> original;
    std::map<std::string, AssemblyModel> catalogs;
    std::map<Target, AssemblyExpansion> roots;
    std::vector<std::unique_ptr<Page>> pages;
    QComboBox* selector{};
    QStackedWidget* stack{};
    QLabel* error{};

    Impl(AssemblyPropertiesPanel* panel, const DocumentSnapshot& captured,
         std::vector<Target> targets, bool metric)
        : owner(panel), source(captured), length_unit(metric ? Unit::metre : Unit::foot),
          original(std::move(targets)) {
        require(!original.empty() && original.size() <= maximum_targets, "Select 1..1000 qualified targets.");
        std::sort(original.begin(), original.end());
        require(std::adjacent_find(original.begin(), original.end()) == original.end(), "Selected targets must be unique.");
        AssemblyExpansionBudget budget;
        for (auto target : original) {
            Intent identity; identity.target = target;
            (void)decode_assembly_instance_property_edit_intent(encode_assembly_instance_property_edit_intent(identity));
            target.part_path.clear();
            if (roots.contains(target)) continue;
            const auto catalog = source.entities().find(target.catalog_id);
            require(catalog != source.entities().end() && catalog->second.id == target.catalog_id &&
                catalog->second.type == "assembly_model", "The selected catalog is missing or inconsistent.");
            auto model = catalogs.find(target.catalog_id);
            if (model == catalogs.end()) model = catalogs.emplace(target.catalog_id,
                AssemblyModel::from_json(catalog->second.properties.at("model"))).first;
            AssemblyInstance instance;
            if (target.document_entity_id) {
                const auto found = source.entities().find(*target.document_entity_id);
                require(found != source.entities().end() && found->second.id == *target.document_entity_id,
                    "The selected independent root is missing or inconsistent.");
                const auto document = decode_document_assembly_instance(found->second);
                require(document.assembly_catalog_id == target.catalog_id && document.instance.id == target.instance_id,
                    "The selected independent root does not match its catalog and instance.");
                instance = document.instance;
            } else {
                const auto found = std::find_if(model->second.instances().begin(), model->second.instances().end(),
                    [&](const auto& row) { return row.id == target.instance_id; });
                require(found != model->second.instances().end(), "The selected embedded instance is missing.");
                instance = *found;
            }
            // Semantic expansion only. No native geometry admission or request
            // preparation is performed while projecting captured UI values.
            roots.emplace(target, model->second.expand(instance, budget));
        }

        auto* layout = new QVBoxLayout(owner);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* bar = new QHBoxLayout;
        bar->addWidget(new QLabel("Assembly part", owner));
        selector = new QComboBox(owner);
        selector->setObjectName("assemblyPropertiesPart");
        selector->setAccessibleName("Assembly part");
        selector->setToolTip("Choose a component part to edit. Entered values remain when you switch parts.");
        bar->addWidget(selector, 1);
        layout->addLayout(bar);
        stack = new QStackedWidget(owner);
        stack->setObjectName("assemblyPropertiesPages");
        layout->addWidget(stack, 1);
        error = new QLabel(owner);
        error->setObjectName("assemblyPropertiesError");
        error->setWordWrap(true);
        error->hide();
        layout->addWidget(error);

        std::set<Path> paths;
        bool first = true;
        for (const auto& target : original) {
            std::set<Path> available;
            for (const auto& node : expansion(target).nodes)
                if (prefix(target.part_path, node.part_path))
                    available.emplace(node.part_path.begin() + target.part_path.size(), node.part_path.end());
            if (first) { paths = std::move(available); first = false; }
            else std::erase_if(paths, [&](const auto& path) { return !available.contains(path); });
        }
        require(paths.contains(Path{}), "An original selected path is absent from its source expansion.");
        add_page(Path{}, std::all_of(original.begin(), original.end(), [](const auto& t) { return t.part_path.empty(); })
            ? QString("Root") : QString("Selected parts"));
        for (const auto& path : paths) if (!path.empty()) add_page(path, part_name(path));
        build(*pages.front());
        stack->setCurrentWidget(pages.front()->widget);
        QObject::connect(selector, &QComboBox::currentIndexChanged, owner, [this](int index) {
            if (index < 0) return;
            try {
                auto& page = *pages.at(static_cast<std::size_t>(index));
                if (!page.widget) build(page);
                stack->setCurrentWidget(page.widget);
                error->clear(); error->hide();
            } catch (const std::exception& failure) {
                error->setText(q(failure.what())); error->show();
            }
        });
    }

    const AssemblyExpansion& expansion(Target target) const {
        target.part_path.clear();
        return roots.at(target);
    }
    Source resolve(const Target& target) const {
        const auto& expanded = expansion(target);
        const auto node = std::find_if(expanded.nodes.begin(), expanded.nodes.end(),
            [&](const auto& row) { return row.part_path == target.part_path; });
        require(node != expanded.nodes.end(), "A selected stable part path is missing.");
        const auto& model = catalogs.at(target.catalog_id);
        const auto type = std::find_if(model.types().begin(), model.types().end(),
            [&](const auto& row) { return row.id == node->type_id; });
        require(type != model.types().end(), "A selected expanded type is missing.");
        auto pose = root_pose(expanded.source_instance);
        if (!target.part_path.empty()) {
            require(node->source_part.has_value(), "A nested target has no source part pose.");
            pose = node->path_override && node->path_override->transform ?
                *node->path_override->transform : node->source_part->transform;
        }
        return {target, &model, &*node, &*type, pose};
    }
    QString part_name(const Path& relative) const {
        QString first;
        bool mixed = false;
        for (auto target : original) {
            QStringList names;
            for (const auto& part : relative) {
                target.part_path.push_back(part);
                const auto node = resolve(target);
                names.push_back(q(node.type->name.empty() ? part : node.type->name));
            }
            const auto label = names.join(" / ");
            if (first.isEmpty()) first = label;
            else if (first != label) mixed = true;
        }
        return first + (mixed ? QString(" (varies)") : QString{});
    }
    void add_page(const Path& relative, const QString& label) {
        auto page = std::make_unique<Page>();
        std::set<Target> unique;
        for (auto target : original) {
            target.part_path.insert(target.part_path.end(), relative.begin(), relative.end());
            require(unique.insert(target).second, "A part page resolves duplicate source targets.");
            page->sources.push_back(resolve(target));
        }
        selector->addItem(label, path_label(relative));
        selector->setItemData(selector->count() - 1, path_label(relative), Qt::ToolTipRole);
        pages.push_back(std::move(page));
    }
    QFormLayout* tab(QTabWidget* tabs, const QString& title) {
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
    QLineEdit* field(QWidget* parent, const QString& name, const QString& hint) {
        auto* result = new QLineEdit(parent);
        result->setObjectName(name);
        result->setAccessibleName(name);
        result->setMaxLength(4096);
        result->setClearButtonEnabled(true);
        result->setPlaceholderText(hint + " (unchanged)");
        result->setToolTip("Current: " + hint);
        return result;
    }
    QComboBox* operations(QWidget* parent, const QString& name, bool clear = true) {
        auto* result = new QComboBox(parent);
        result->setObjectName(name + "Operation");
        result->setAccessibleName(name + " operation");
        result->addItem("Keep", static_cast<int>(Operation::unchanged));
        result->addItem("Set", static_cast<int>(Operation::set));
        if (clear) result->addItem("Use definition", static_cast<int>(Operation::clear));
        result->setToolTip("Keep retains each current value. Use definition removes this instance's override.");
        return result;
    }
    ValueRow row(QFormLayout* form, const QString& label, const std::string& key,
                 const QString& hint, const QString& family) {
        auto* holder = new QWidget(owner);
        auto* layout = new QHBoxLayout(holder);
        layout->setContentsMargins(0, 0, 0, 0);
        ValueRow result;
        result.key = key;
        result.operation = operations(holder, "assembly" + family + q(key));
        result.text = field(holder, label, hint);
        result.text->setEnabled(false);
        layout->addWidget(result.operation);
        layout->addWidget(result.text, 1);
        QObject::connect(result.operation, &QComboBox::currentIndexChanged, owner,
            [op = result.operation, input = result.text](int) { input->setEnabled(operation(op) == Operation::set); });
        form->addRow(label, holder);
        return result;
    }
    void build(Page& page) {
        auto* tabs = new QTabWidget(stack);
        const auto properties = common_slots(page.sources, [](const auto& type) -> const auto& { return type.properties; },
            [](const auto&, const auto&) { return true; });
        auto* form = tab(tabs, "Properties");
        for (const auto& [key, value] : properties) {
            (void)value;
            const auto hint = common_hint<std::string>(page.sources,
                [](const auto& node) -> const auto& { return node.properties; }, key, [](const auto& v) { return q(v); });
            auto input = row(form, q(key), key, hint, "Property");
            input.text->setToolTip("Current: " + hint + "\nSet accepts an empty string. Keep preserves each selected value.");
            page.properties.push_back(input);
        }
        if (properties.empty()) form->addRow(new QLabel("No common declared properties.", tabs));

        const auto materials = common_slots(page.sources, [](const auto& type) -> const auto& { return type.materials; },
            [](const auto&, const auto&) { return true; });
        std::map<std::string, QStringList> choices;
        for (const auto& material : page.sources.front().model->materials())
            choices.emplace(material.id, QStringList{q(material.name)});
        for (const auto& selected : page.sources) {
            std::erase_if(choices, [&](const auto& choice) {
                return std::none_of(selected.model->materials().begin(), selected.model->materials().end(),
                    [&](const auto& material) { return material.id == choice.first; });
            });
            for (const auto& material : selected.model->materials())
                if (auto found = choices.find(material.id); found != choices.end() && !found->second.contains(q(material.name)))
                    found->second.push_back(q(material.name));
        }
        form = tab(tabs, "Materials");
        for (const auto& [key, value] : materials) {
            (void)value;
            const auto first = page.sources.front().node->materials.at(key);
            QString hint;
            for (const auto& selected : page.sources) {
                const auto& id = selected.node->materials.at(key);
                if (id != first) { hint = "Multiple values"; break; }
                const auto material = std::find_if(selected.model->materials().begin(), selected.model->materials().end(),
                    [&](const auto& m) { return m.id == id; });
                require(material != selected.model->materials().end(), "An effective material is missing from its catalog.");
                if (hint.isEmpty()) hint = q(material->name);
                else if (hint != q(material->name)) { hint = "Multiple material names"; break; }
            }
            auto* holder = new QWidget(owner);
            auto* layout = new QHBoxLayout(holder);
            layout->setContentsMargins(0, 0, 0, 0);
            ValueRow input;
            input.key = key;
            input.operation = operations(holder, "assemblyMaterial" + q(key));
            input.material = new QComboBox(holder);
            input.material->setObjectName("assemblyMaterial" + q(key));
            input.material->setAccessibleName(q(key));
            input.material->addItem("Choose material", QString());
            for (const auto& [id, names] : choices) {
                input.material->addItem(names.join(" / "), q(id));
                input.material->setItemData(input.material->count() - 1, q(id), Qt::ToolTipRole);
            }
            input.material->setToolTip("Current: " + hint + "\nChoices are valid in every selected source catalog.");
            input.material->setEnabled(false);
            input.operation->setItemText(0, "Keep (" + hint + ")");
            QObject::connect(input.operation, &QComboBox::currentIndexChanged, owner,
                [op = input.operation, choice = input.material](int) { choice->setEnabled(operation(op) == Operation::set); });
            layout->addWidget(input.operation); layout->addWidget(input.material, 1);
            form->addRow(q(key), holder);
            page.materials.push_back(input);
        }
        if (materials.empty()) form->addRow(new QLabel("No common declared material slots.", tabs));

        const auto quantities = common_slots(page.sources, [](const auto& type) -> const auto& { return type.quantities; },
            [](const auto& a, const auto& b) { return a.unit == b.unit; });
        form = tab(tabs, "Quantities");
        for (const auto& [key, value] : quantities) {
            const auto hint = common_hint<AssemblyQuantityProperty>(page.sources,
                [](const auto& node) -> const auto& { return node.quantities; }, key,
                [](const auto& v) { return exact(v.value) + " " + unit_name(v.unit); });
            auto input = row(form, q(key) + " (" + unit_name(value.unit) + ")", key, hint, "Quantity");
            input.unit = value.unit;
            input.text->setToolTip("Current: " + hint + (value.unit == AssemblyQuantityUnit::metre ?
                "\nEnter a length with units; bare numbers use metres." :
                "\nEnter a nonnegative number in the labeled SI unit. Counts must be whole numbers."));
            page.quantities.push_back(input);
        }
        if (quantities.empty()) form->addRow(new QLabel("No common declared quantities with matching units.", tabs));

        form = tab(tabs, "Pose");
        const bool nested = std::all_of(page.sources.begin(), page.sources.end(), [](const auto& s) { return !s.target.part_path.empty(); });
        page.pose_operation = operations(tabs, "assemblyPose", nested);
        if (nested) page.pose_operation->setItemText(2, "Use part pose");
        page.pose_operation->setToolTip(nested ? "Use part pose removes the local instance pose override." : "Root poses support Keep and Set.");
        form->addRow("Pose operation", page.pose_operation);
        const std::array<QString, 6> labels{"X", "Y", "Z", "Angle (degrees)", "XY scale", "Vertical scale multiplier"};
        const auto components = [](const AssemblyTransform& p) {
            return std::array<double, 6>{p.translation_m.x, p.translation_m.y, p.translation_m.z,
                p.rotation_radians, p.scale, p.vertical_scale};
        };
        const auto first = components(page.sources.front().pose);
        for (std::size_t i = 0; i < page.pose.size(); ++i) {
            const bool mixed = std::any_of(page.sources.begin(), page.sources.end(), [&](const auto& s) { return components(s.pose)[i] != first[i]; });
            QString hint = "Multiple values";
            if (!mixed) hint = i < 3 ? exact(length_unit == Unit::metre ? first[i] : first[i] / 0.3048) +
                (length_unit == Unit::metre ? " m" : " ft") : i == 3 ? exact(first[i] * 180 / std::numbers::pi) + " deg" : exact(first[i]);
            page.pose[i] = field(tabs, "assemblyPose" + QString::number(i), hint);
            page.pose[i]->setAccessibleName(labels[i]);
            page.pose[i]->setEnabled(false);
            page.pose[i]->setToolTip("Current: " + hint + (i < 3 ?
                (length_unit == Unit::metre ? "\nBlank keeps each value. Lengths accept units; bare numbers use metres." :
                    "\nBlank keeps each value. Lengths accept units; bare numbers use feet.") :
                i == 3 ? "\nBlank keeps each value. Enter a finite angle in degrees." :
                "\nBlank keeps each value. Enter a positive multiplier. Z scale is XY scale times vertical multiplier."));
            form->addRow(labels[i], page.pose[i]);
        }
        page.mirrored = new QComboBox(tabs);
        page.mirrored->setObjectName("assemblyPoseMirrored");
        page.mirrored->setAccessibleName("Mirrored Y");
        const bool mirror_mixed = std::any_of(page.sources.begin(), page.sources.end(), [&](const auto& s) { return s.pose.mirrored_y != page.sources.front().pose.mirrored_y; });
        page.mirrored->addItem(mirror_mixed ? "Keep (mixed)" : page.sources.front().pose.mirrored_y ? "Keep (yes)" : "Keep (no)");
        page.mirrored->addItem("Yes", true); page.mirrored->addItem("No", false);
        page.mirrored->setEnabled(false);
        form->addRow("Mirrored Y", page.mirrored);
        QObject::connect(page.pose_operation, &QComboBox::currentIndexChanged, owner, [page_ptr = &page](int) {
            const bool enabled = operation(page_ptr->pose_operation) == Operation::set;
            for (auto* input : page_ptr->pose) input->setEnabled(enabled);
            page_ptr->mirrored->setEnabled(enabled);
        });
        page.widget = tabs;
        stack->addWidget(tabs);
    }

    std::vector<Intent> read(const Page& page) const {
        Intent shared;
        for (const auto& row : page.properties) {
            auto& patch = shared.properties[row.key];
            patch.operation = operation(row.operation);
            if (patch.operation == Operation::set) patch.value = row.text->text().toStdString();
        }
        for (const auto& row : page.materials) {
            auto& patch = shared.materials[row.key];
            patch.operation = operation(row.operation);
            if (patch.operation == Operation::set) {
                require(row.material->currentIndex() > 0, "Choose a material for " + row.key + ".");
                patch.value = row.material->currentData().toString().toStdString();
            }
        }
        for (const auto& row : page.quantities) {
            auto& patch = shared.quantities[row.key];
            patch.operation = operation(row.operation);
            if (patch.operation == Operation::set) {
                const auto value = row.unit == AssemblyQuantityUnit::metre ?
                    parse_quantity(row.text->text().toStdString(), Unit::metre).metres : scalar(row.text);
                require(std::isfinite(value) && value >= 0 &&
                    (row.unit != AssemblyQuantityUnit::count || std::floor(value) == value),
                    row.key + " must be nonnegative; counts must be whole numbers.");
                patch.value = AssemblyQuantityProperty{value, row.unit};
            }
        }
        const auto pose_operation = operation(page.pose_operation);
        std::array<std::optional<double>, 6> values;
        if (pose_operation == Operation::set) {
            for (std::size_t i = 0; i < values.size(); ++i) if (supplied(page.pose[i])) {
                values[i] = i < 3 ? parse_quantity(page.pose[i]->text().toStdString(), length_unit).metres :
                    i == 3 ? scalar(page.pose[i]) * (std::numbers::pi / 180.0) : scalar(page.pose[i], true);
                require(std::isfinite(*values[i]), "Pose components must be finite.");
            }
        }
        std::vector<Intent> result;
        for (const auto& selected : page.sources) {
            auto intent = shared;
            intent.target = selected.target;
            if (pose_operation == Operation::clear) intent.pose.operation = Operation::clear;
            else if (pose_operation == Operation::set) {
                auto& pose = intent.pose;
                // Explicit values must also replace a pose carried by another edit,
                // even when they match the captured source pose.
                pose.x_metres = values[0];
                pose.y_metres = values[1];
                pose.z_metres = values[2];
                pose.rotation_radians = values[3];
                pose.scale = values[4];
                pose.vertical_scale = values[5];
                if (page.mirrored->currentIndex() > 0)
                    pose.mirrored_y = page.mirrored->currentData().toBool();
                if (pose.x_metres || pose.y_metres || pose.z_metres || pose.rotation_radians || pose.scale || pose.vertical_scale || pose.mirrored_y)
                    pose.operation = Operation::set;
                const auto xy = pose.scale.value_or(selected.pose.scale);
                const auto vertical = pose.vertical_scale.value_or(selected.pose.vertical_scale);
                require(std::isfinite(xy * vertical) && xy * vertical > 0, "The resulting Z scale overflows or underflows.");
            }
            result.push_back(std::move(intent));
        }
        return result;
    }
    std::vector<Intent> intents() const {
        std::map<Target, Intent> result;
        for (const auto& target : original) { Intent retained; retained.target = target; result.emplace(target, std::move(retained)); }
        for (std::size_t index = 0; index < pages.size(); ++index) {
            if (!pages[index]->widget) continue;
            auto values = read(*pages[index]);
            if (index != 0 && std::none_of(values.begin(), values.end(), changed)) continue;
            for (auto& intent : values) {
                const auto found = result.find(intent.target);
                if (found != result.end() && changed(found->second)) {
                    require(!changed(intent), "A source part was edited on more than one page. Keep its edits on one page.");
                    continue;
                }
                const auto target = intent.target;
                result.insert_or_assign(target, std::move(intent));
                require(result.size() <= maximum_targets, "The combined selection and edited parts exceed 1000 targets.");
            }
        }
        std::vector<Intent> output;
        output.reserve(result.size());
        for (const auto& [target, intent] : result) {
            (void)target;
            // The producer codec validates the complete typed envelope without
            // invoking preparation, native admission or changing the document.
            output.push_back(decode_assembly_instance_property_edit_intent(
                encode_assembly_instance_property_edit_intent(intent)));
        }
        return output;
    }
};

AssemblyPropertiesPanel::AssemblyPropertiesPanel(const DocumentSnapshot& captured,
    std::vector<Target> targets, bool metric, QWidget* parent) : QWidget(parent),
    impl_(std::make_unique<Impl>(this, captured, std::move(targets), metric)) {}
AssemblyPropertiesPanel::~AssemblyPropertiesPanel() = default;
std::vector<Intent> AssemblyPropertiesPanel::intents() const {
    try {
        auto result = impl_->intents();
        impl_->error->clear(); impl_->error->hide();
        return result;
    } catch (const std::exception& failure) {
        impl_->error->setText(q(failure.what())); impl_->error->show();
        throw;
    }
}
QString AssemblyPropertiesPanel::lastError() const { return impl_->error->text(); }

} // namespace sketch::desktop
