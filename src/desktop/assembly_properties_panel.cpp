#include "sketch/desktop/assembly_properties_panel.hpp"
#include "sketch/desktop/assembly_authoring_dialog.hpp"

#include "sketch/assembly_profile_geometry.hpp"
#include "sketch/quantity.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardItemModel>
#include <QStackedWidget>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
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
constexpr std::size_t maximum_profiles = 4096;

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
        intent.pose.operation != Operation::unchanged || intent.profile_geometry.operation != Operation::unchanged;
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
    const std::vector<AssemblyProfile>* profiles{};
};
struct ValueRow {
    std::string key;
    QComboBox* operation{};
    QLineEdit* text{};
    QComboBox* material{};
    AssemblyQuantityUnit unit{AssemblyQuantityUnit::count};
};
struct ProfileRow {
    AssemblyProfile draft;
    QLineEdit *elevation{}, *height{};
    QComboBox* material_slot{};
    QLabel* summary{};
    QPushButton* edit{};
};
struct ProfileCollection {
    std::size_t source_index{};
    QWidget* widget{};
    QTableWidget* table{};
    QPushButton *add{}, *remove{}, *enable_dimensions{};
    QLabel* topology_note{};
    bool initialize_profile_topology{false};
    std::vector<ProfileRow> profiles;
    // Retain removed identities for the entire dialog lifetime, including IDs
    // in the shared type hidden by an existing instance override.
    std::set<std::string> reserved_ids;
};
struct Page {
    std::vector<Source> sources;
    QWidget* widget{};
    std::vector<ValueRow> properties, materials, quantities;
    QComboBox *pose_operation{}, *mirrored{};
    std::array<QLineEdit*, 6> pose{};
    QComboBox* geometry_operation{};
    std::array<QLineEdit*, 3> dimensions{};
    QComboBox* profile_source{};
    QStackedWidget* profile_stack{};
    std::vector<std::unique_ptr<ProfileCollection>> profile_collections;
};

bool identified(const std::vector<AssemblyProfile>& profiles) {
    return std::any_of(profiles.begin(), profiles.end(), [](const auto& profile) { return profile.topology.has_value(); });
}

bool same_boundary(const Boundary& first, const Boundary& second) {
    return first.size() == second.size() && std::equal(first.begin(), first.end(), second.begin(),
        [](const auto& a, const auto& b) {
            return a.start.x == b.start.x && a.start.y == b.start.y &&
                a.end.x == b.end.x && a.end.y == b.end.y && a.sweep_radians == b.sweep_radians;
        });
}
bool same_profile_geometry(const AssemblyProfile& first, const AssemblyProfile& second) {
    return same_boundary(first.outer, second.outer) && first.holes.size() == second.holes.size() &&
        std::equal(first.holes.begin(), first.holes.end(), second.holes.begin(), same_boundary);
}
void assign_fresh_topology(AssemblyProfile& profile, const std::vector<AssemblyProfile>& own_profiles,
                           const std::vector<AssemblyProfile>& type_profiles) {
    std::set<std::string> issued;
    // Reserve every identity from the actual same-ID own and hidden type
    // profiles. Equal edge counts never authorize reusing old identities.
    const auto reserve_profile = [&](const std::vector<AssemblyProfile>& profiles) {
        const auto found = std::find_if(profiles.begin(), profiles.end(),
            [&](const auto& value) { return value.id == profile.id; });
        if (found == profiles.end() || !found->topology) return;
        const auto reserve = [&](const AssemblyProfileRingIdentity& ring) {
            issued.insert(ring.ring_id);
            issued.insert(ring.segment_ids.begin(), ring.segment_ids.end());
            issued.insert(ring.vertex_ids.begin(), ring.vertex_ids.end());
        };
        reserve(found->topology->outer);
        for (const auto& hole : found->topology->holes) reserve(hole);
    };
    reserve_profile(own_profiles);
    reserve_profile(type_profiles);
    const auto fresh = [&] {
        std::string id;
        do { id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(); } while (!issued.insert(id).second);
        return id;
    };
    const auto ring = [&](const Boundary& boundary) {
        AssemblyProfileRingIdentity result;
        result.ring_id = fresh();
        result.segment_ids.reserve(boundary.size());
        result.vertex_ids.reserve(boundary.size());
        for (std::size_t i = 0; i < boundary.size(); ++i) {
            result.segment_ids.push_back(fresh());
            result.vertex_ids.push_back(fresh());
        }
        return result;
    };
    AssemblyProfileTopology result;
    result.outer = ring(profile.outer);
    result.holes.reserve(profile.holes.size());
    for (const auto& hole : profile.holes) result.holes.push_back(ring(hole));
    profile.topology = std::move(result);
    validate_assembly_profile_topology(profile);
}
void initialize_topology(AssemblyProfile& profile, const std::vector<AssemblyProfile>& type_profiles) {
    if (!profile.topology) assign_fresh_topology(profile, {}, type_profiles);
}
void collect_reviewed_topology(std::vector<AssemblyProfile>& profiles, const Source& selected) {
    for (auto& profile : profiles) {
        const auto actual = std::find_if(selected.profiles->begin(), selected.profiles->end(),
            [&](const auto& value) { return value.id == profile.id; });
        if (actual != selected.profiles->end() && same_profile_geometry(*actual, profile))
            profile.topology = actual->topology;
        else
            assign_fresh_topology(profile, *selected.profiles, selected.type->profiles);
    }
}

struct ProfileBounds {
    Vec2 minimum, maximum;
    double bottom{}, top{};
};
ProfileBounds profile_bounds(const std::vector<AssemblyProfile>& profiles) {
    require(!profiles.empty(), "This node has no own profiles to measure or resize. Add a profile or leave its dimensions blank.");
    ProfileBounds result;
    bool first = true;
    for (const auto& profile : profiles) {
        const auto bounds = boundary_bounds(profile.outer);
        const auto top = profile.elevation_m + profile.height_m;
        require(std::isfinite(top), "Profile extrusion endpoints must be representable.");
        if (first) {
            result = {bounds.minimum, bounds.maximum, profile.elevation_m, top};
            first = false;
        } else {
            result.minimum.x = std::min(result.minimum.x, bounds.minimum.x);
            result.minimum.y = std::min(result.minimum.y, bounds.minimum.y);
            result.maximum.x = std::max(result.maximum.x, bounds.maximum.x);
            result.maximum.y = std::max(result.maximum.y, bounds.maximum.y);
            result.bottom = std::min(result.bottom, profile.elevation_m);
            result.top = std::max(result.top, top);
        }
    }
    return result;
}
std::array<double, 3> profile_spans(const std::vector<AssemblyProfile>& profiles, const AssemblyTransform& frame) {
    const auto bounds = profile_bounds(profiles);
    const auto z_scale = frame.scale * frame.vertical_scale;
    require(std::isfinite(frame.scale) && frame.scale > 0 && std::isfinite(z_scale) && z_scale > 0,
        "The geometry pose scales must be finite, positive and representable.");
    std::array<double, 3> result{(bounds.maximum.x - bounds.minimum.x) * frame.scale,
        (bounds.maximum.y - bounds.minimum.y) * frame.scale,
        (bounds.top - bounds.bottom) * z_scale};
    for (const auto span : result)
        require(std::isfinite(span) && span > 0, "The own-profile dimensions must be finite, positive and representable.");
    return result;
}
bool curved(const std::vector<AssemblyProfile>& profiles) {
    const auto has_arc = [](const Boundary& boundary) {
        return std::any_of(boundary.begin(), boundary.end(), [](const auto& edge) { return edge.sweep_radians != 0; });
    };
    return std::any_of(profiles.begin(), profiles.end(), [&](const auto& profile) {
        return has_arc(profile.outer) || std::any_of(profile.holes.begin(), profile.holes.end(), has_arc);
    });
}
void resize_profiles(std::vector<AssemblyProfile>& profiles, const AssemblyTransform& frame,
                     const std::array<std::optional<double>, 3>& dimensions) {
    if (std::none_of(dimensions.begin(), dimensions.end(), [](const auto& value) { return value.has_value(); })) return;
    const auto bounds = profile_bounds(profiles);
    const auto spans = profile_spans(profiles, frame);
    std::array<double, 3> factors{1, 1, 1};
    for (std::size_t i = 0; i < factors.size(); ++i) {
        if (dimensions[i]) factors[i] = *dimensions[i] / spans[i];
        require(std::isfinite(factors[i]) && factors[i] > 0, "Geometry resize factors must be finite and positive.");
    }
    if (curved(profiles) && factors[0] != factors[1]) {
        // Circular arcs can only undergo a planar similarity. Allow arithmetic
        // rounding in an otherwise uniform request, then use ONE exact factor.
        const auto rounding = 32 * std::numeric_limits<double>::epsilon() * std::max(factors[0], factors[1]);
        require(std::abs(factors[0] - factors[1]) <= rounding,
            "Circular arcs require equal width and depth scale factors. Enter both dimensions in their current ratio, "
            "or edit the outline/cutouts explicitly. Unequal planar scaling would require unsupported elliptical arcs.");
        factors[1] = factors[0];
    }
    const AssemblyProfilePlanarAffine planar_resize{bounds.minimum, bounds.minimum,
        factors[0], 0.0, 0.0, factors[1]};
    validate_assembly_profile_affine(planar_resize);
    const auto scale_boundary = [&](Boundary& boundary) {
        if (factors[0] == 1 && factors[1] == 1) return;
        // This historical endpoint path admits lines and planar similarities
        // only. A general affine circular image needs the mapped-profile codec
        // and every exact-curve consumer before it can be authored here.
        for (auto& edge : boundary) {
            edge.start = assembly_profile_affine_point(edge.start, planar_resize);
            edge.end = assembly_profile_affine_point(edge.end, planar_resize);
        }
    };
    for (auto& profile : profiles) {
        scale_boundary(profile.outer);
        for (auto& hole : profile.holes) scale_boundary(hole);
        if (factors[2] != 1) {
            profile.elevation_m = bounds.bottom + (profile.elevation_m - bounds.bottom) * factors[2];
            profile.height_m *= factors[2];
        }
    }
}
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
        const auto* profiles = expanded.source_instance.profile_overrides ?
            &*expanded.source_instance.profile_overrides : &type->profiles;
        if (!target.part_path.empty()) {
            require(node->source_part.has_value(), "A nested target has no source part pose.");
            pose = node->path_override && node->path_override->transform ?
                *node->path_override->transform : node->source_part->transform;
            profiles = node->path_override && node->path_override->profile_overrides ?
                &*node->path_override->profile_overrides : &type->profiles;
        }
        return {target, &model, &*node, &*type, pose, profiles};
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
    QString length(double metres) const {
        return exact(length_unit == Unit::metre ? metres : metres / 0.3048) +
            (length_unit == Unit::metre ? " m" : " ft");
    }
    QString profile_summary(const AssemblyProfile& profile) const {
        return QString("%1 outline edges; %2 cutouts%3").arg(static_cast<qulonglong>(profile.outer.size()))
            .arg(static_cast<qulonglong>(profile.holes.size()))
            .arg(profile.topology ? "; edge dimensions enabled" : "");
    }
    std::vector<AssemblyProfile> profile_draft(const ProfileCollection& collection) const {
        std::vector<AssemblyProfile> result;
        result.reserve(collection.profiles.size());
        for (const auto& row : collection.profiles) {
            auto profile = row.draft;
            try {
                if (supplied(row.elevation)) profile.elevation_m = parse_quantity(row.elevation->text().toStdString(), length_unit).metres;
                if (supplied(row.height)) profile.height_m = parse_quantity(row.height->text().toStdString(), length_unit).metres;
                require(std::isfinite(profile.elevation_m) && std::isfinite(profile.height_m) && profile.height_m > 0,
                    "Elevation must be finite and extrusion height must be finite and positive.");
                if (row.material_slot->currentIndex() > 0) {
                    const auto slot = row.material_slot->currentData().toString().toStdString();
                    profile.material_slot = slot.empty() ? std::nullopt : std::optional<std::string>{slot};
                }
            } catch (const std::exception& failure) {
                throw std::invalid_argument("Profile " + profile.id + ": " + failure.what());
            }
            result.push_back(std::move(profile));
        }
        return result;
    }
    QString topology_initialization_refusal(const Page& page, const ProfileCollection& collection) const {
        const auto& actual = *page.sources[collection.source_index].profiles;
        if (collection.initialize_profile_topology) return "Apply these edge identities, then reopen Properties to edit profiles.";
        if (std::none_of(actual.begin(), actual.end(), [](const auto& profile) { return !profile.topology; }))
            return actual.empty() ? "This component has no own profiles to enable." : "Edge dimensions are already enabled for all own profiles.";
        if (std::any_of(page.dimensions.begin(), page.dimensions.end(), supplied))
            return "Apply or clear dimension changes before enabling edge dimensions.";
        try {
            if (profile_draft(collection) != actual)
                return "Apply profile changes first, then reopen Properties to enable edge dimensions on that geometry.";
        } catch (const std::exception&) {
            return "Correct or clear profile values before enabling edge dimensions.";
        }
        return {};
    }
    void update_geometry_controls(Page& page) {
        const bool pending = std::any_of(page.profile_collections.begin(), page.profile_collections.end(),
            [](const auto& collection) { return collection && collection->initialize_profile_topology; });
        page.geometry_operation->setEnabled(!pending);
        if (auto* model = qobject_cast<QStandardItemModel*>(page.geometry_operation->model())) {
            if (auto* item = model->item(2)) {
                item->setEnabled(!pending);
                item->setToolTip("Return to this component's exact type geometry. You can review affected dimensions before applying.");
            }
        }
        const bool enabled = operation(page.geometry_operation) == Operation::set;
        for (auto* dimension : page.dimensions)
            dimension->setEnabled(enabled && !pending);
        page.profile_stack->setEnabled(enabled);
        for (auto& collection : page.profile_collections) {
            if (!collection) continue;
            const bool frozen = collection->initialize_profile_topology;
            const bool mapped = identified(*page.sources[collection->source_index].profiles);
            collection->add->setEnabled(!frozen && collection->profiles.size() < maximum_profiles);
            collection->remove->setEnabled(!frozen && collection->table->currentRow() >= 0 &&
                !collection->table->selectedItems().empty());
            for (auto& row : collection->profiles) {
                row.edit->setEnabled(!frozen);
                row.elevation->setEnabled(!frozen);
                row.height->setEnabled(!frozen);
                row.material_slot->setEnabled(!frozen);
            }
            const auto refusal = topology_initialization_refusal(page, *collection);
            collection->enable_dimensions->setEnabled(refusal.isEmpty());
            collection->enable_dimensions->setToolTip(refusal.isEmpty() ?
                "Assign fresh edge and vertex identities to this component's unchanged own profiles. Apply, then add edge dimensions." : refusal);
            auto note = refusal;
            if (frozen)
                note = "Edge identities are ready. Apply and reopen Properties before changing geometry, extrusion or profile material slots.";
            else if (mapped)
                note += (note.isEmpty() ? QString{} : "\n") +
                    QString("You can edit outlines, cutouts, the profile collection and dimensions, or use type geometry. "
                            "You can review affected dimensions before applying.");
            else if (note.isEmpty())
                note = "Enable edge dimensions assigns identities to the captured own profiles without changing their geometry. Apply before editing them.";
            if (!frozen && pending)
                note += "\nApply the pending edge identities on this page before changing its common dimensions or geometry operation.";
            collection->topology_note->setText(note);
        }
    }
    void append_profile(ProfileCollection& collection, const Source& selected, AssemblyProfile profile, Page& page) {
        require(collection.profiles.size() < maximum_profiles, "This node already has 4096 own profiles.");
        auto* table = collection.table;
        const auto row_index = table->rowCount();
        table->insertRow(row_index);
        ProfileRow row;
        row.draft = std::move(profile);
        const auto& draft = row.draft;
        auto* id = new QTableWidgetItem(q(draft.id));
        id->setToolTip("Stable local profile identity: " + q(draft.id));
        table->setItem(row_index, 0, id);
        row.summary = new QLabel(profile_summary(draft), table);
        table->setCellWidget(row_index, 1, row.summary);
        row.elevation = field(table, "assemblyProfileElevation" + q(draft.id), length(draft.elevation_m));
        row.height = field(table, "assemblyProfileHeight" + q(draft.id), length(draft.height_m));
        row.elevation->setAccessibleName("Profile " + q(draft.id) + " local elevation");
        row.height->setAccessibleName("Profile " + q(draft.id) + " local extrusion height");
        const auto units = length_unit == Unit::metre ? QString("Bare numbers use metres.") : QString("Bare numbers use feet.");
        row.elevation->setToolTip("Current local elevation: " + length(draft.elevation_m) + "\nBlank retains it. " + units);
        row.height->setToolTip("Current local height: " + length(draft.height_m) + "\nEnter a positive length; blank retains it. " + units);
        table->setCellWidget(row_index, 2, row.elevation);
        table->setCellWidget(row_index, 3, row.height);
        row.material_slot = new QComboBox(table);
        row.material_slot->setObjectName("assemblyProfileMaterialSlot" + q(draft.id));
        row.material_slot->setAccessibleName("Profile " + q(draft.id) + " material slot");
        row.material_slot->addItem(draft.material_slot ? "Keep (" + q(*draft.material_slot) + ")" : "Keep (no material slot)");
        row.material_slot->addItem("No material slot", QString{});
        for (const auto& [slot, unused] : selected.type->materials) {
            (void)unused;
            row.material_slot->addItem(q(slot), q(slot));
            const auto material = std::find_if(selected.model->materials().begin(), selected.model->materials().end(),
                [&](const auto& value) { return value.id == selected.node->materials.at(slot); });
            require(material != selected.model->materials().end(), "A profile material slot has no actual resolved material.");
            row.material_slot->setItemData(row.material_slot->count() - 1,
                "Current material: " + q(material->name), Qt::ToolTipRole);
        }
        row.material_slot->setToolTip("Choose one of this component type's declared slots. Materials are resolved through "
            "this node's overrides; use the Materials tab to change their material. Keep retains this profile's slot.");
        table->setCellWidget(row_index, 4, row.material_slot);
        row.edit = new QPushButton("Edit outline and cutouts…", table);
        row.edit->setObjectName("assemblyProfileEdit" + q(draft.id));
        row.edit->setAccessibleName("Edit profile " + q(draft.id) + " outline and cutouts");
        table->setCellWidget(row_index, 5, row.edit);
        const auto profile_id = draft.id;
        collection.reserved_ids.insert(profile_id);
        collection.profiles.push_back(std::move(row));
        QObject::connect(collection.profiles.back().edit, &QPushButton::clicked, owner,
            [this, collection_ptr = &collection, page_ptr = &page, profile_id] {
                try {
                    require(!collection_ptr->initialize_profile_topology,
                        "Apply pending edge identities before editing outlines or cutouts.");
                    const auto found = std::find_if(collection_ptr->profiles.begin(), collection_ptr->profiles.end(),
                        [&](const auto& value) { return value.draft.id == profile_id; });
                    require(found != collection_ptr->profiles.end(), "The selected profile is no longer in this draft.");
                    // Boundary editing is independent of incomplete extrusion
                    // text and pending material choices. Retain both controls.
                    if (auto accepted = edit_assembly_profile_geometry(owner, found->draft, length_unit == Unit::metre)) {
                        require(accepted->id == found->draft.id && accepted->material_slot == found->draft.material_slot,
                            "The outline editor changed a retained profile identity or material slot.");
                        found->draft.outer = std::move(accepted->outer);
                        found->draft.holes = std::move(accepted->holes);
                        found->summary->setText(profile_summary(found->draft));
                        update_geometry_controls(*page_ptr);
                        error->clear(); error->hide();
                    }
                } catch (const std::exception& failure) { error->setText(q(failure.what())); error->show(); }
            });
        const auto refresh = [this, page_ptr = &page] { update_geometry_controls(*page_ptr); };
        QObject::connect(collection.profiles.back().elevation, &QLineEdit::textChanged, owner, refresh);
        QObject::connect(collection.profiles.back().height, &QLineEdit::textChanged, owner, refresh);
        QObject::connect(collection.profiles.back().material_slot, &QComboBox::currentIndexChanged, owner, refresh);
        table->resizeRowToContents(row_index);
        collection.add->setEnabled(collection.profiles.size() < maximum_profiles);
    }
    void show_profile_collection(Page& page, std::size_t index) {
        require(index < page.sources.size(), "Choose an actual selected component.");
        if (!page.profile_collections[index]) {
            auto collection = std::make_unique<ProfileCollection>();
            collection->source_index = index;
            const auto& selected = page.sources[index];
            collection->widget = new QWidget(page.profile_stack);
            auto* layout = new QVBoxLayout(collection->widget);
            layout->setContentsMargins(0, 0, 0, 0);
            collection->table = new QTableWidget(collection->widget);
            auto* table = collection->table;
            table->setObjectName("assemblyGeometryProfiles");
            table->setAccessibleName("This component's own geometry profiles");
            table->setColumnCount(6);
            table->setHorizontalHeaderLabels({"Profile ID", "Outline / cutouts", "Local elevation", "Local height", "Material slot", "Geometry"});
            table->setSelectionBehavior(QAbstractItemView::SelectRows);
            table->setSelectionMode(QAbstractItemView::SingleSelection);
            table->setEditTriggers(QAbstractItemView::NoEditTriggers);
            table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
            table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
            table->verticalHeader()->hide();
            layout->addWidget(table);
            auto* actions = new QHBoxLayout;
            collection->add = new QPushButton("Add profile…", collection->widget);
            collection->add->setObjectName("assemblyProfileAdd");
            collection->add->setAccessibleName("Add an own profile to this component");
            collection->add->setToolTip("Create a new local profile with a fresh ID using the complete outline and cutout editor. "
                "Its initial local elevation is zero and extrusion height is one metre; edit them in the new row.");
            collection->remove = new QPushButton("Remove selected profile", collection->widget);
            collection->remove->setObjectName("assemblyProfileRemove");
            collection->remove->setAccessibleName("Remove this component's selected own profile");
            collection->remove->setToolTip("Remove only this profile from the local draft. You can review affected dimensions before applying. "
                "Removing the last solid is refused.");
            collection->remove->setEnabled(false);
            collection->enable_dimensions = new QPushButton("Enable edge dimensions", collection->widget);
            collection->enable_dimensions->setObjectName("assemblyProfileEnableEdgeDimensions");
            collection->enable_dimensions->setAccessibleName("Enable edge dimensions for this component's own profiles");
            actions->addWidget(collection->add); actions->addWidget(collection->remove);
            actions->addWidget(collection->enable_dimensions); actions->addStretch();
            layout->addLayout(actions);
            collection->topology_note = new QLabel(collection->widget);
            collection->topology_note->setObjectName("assemblyProfileTopologyNote");
            collection->topology_note->setWordWrap(true);
            layout->addWidget(collection->topology_note);
            for (const auto& profile : selected.type->profiles) collection->reserved_ids.insert(profile.id);
            collection->profiles.reserve(selected.profiles->size());
            for (const auto& profile : *selected.profiles) append_profile(*collection, selected, profile, page);
            auto* collection_ptr = collection.get();
            QObject::connect(table, &QTableWidget::itemSelectionChanged, owner, [this, page_ptr = &page] {
                update_geometry_controls(*page_ptr);
            });
            QObject::connect(collection->enable_dimensions, &QPushButton::clicked, owner, [this, page_ptr = &page, collection_ptr] {
                try {
                    const auto refusal = topology_initialization_refusal(*page_ptr, *collection_ptr);
                    require(refusal.isEmpty(), refusal.toStdString());
                    // Identity assignment is only for these exact captured own
                    // profiles, never positions in an edited or resized draft.
                    const auto& selected = page_ptr->sources[collection_ptr->source_index];
                    auto initialized = *selected.profiles;
                    for (auto& profile : initialized) initialize_topology(profile, selected.type->profiles);
                    (void)validate_assembly_profiles(initialized);
                    require(initialized.size() == collection_ptr->profiles.size(), "The captured own-profile roster changed.");
                    for (std::size_t i = 0; i < initialized.size(); ++i) {
                        collection_ptr->profiles[i].draft = std::move(initialized[i]);
                        collection_ptr->profiles[i].summary->setText(profile_summary(collection_ptr->profiles[i].draft));
                    }
                    collection_ptr->initialize_profile_topology = true;
                    page_ptr->geometry_operation->setCurrentIndex(page_ptr->geometry_operation->findData(static_cast<int>(Operation::set)));
                    update_geometry_controls(*page_ptr);
                    error->clear(); error->hide();
                } catch (const std::exception& failure) { error->setText(q(failure.what())); error->show(); }
            });
            QObject::connect(collection->add, &QPushButton::clicked, owner, [this, page_ptr = &page, collection_ptr] {
                try {
                    require(!collection_ptr->initialize_profile_topology,
                        "Apply pending edge identities before adding profiles.");
                    require(collection_ptr->profiles.size() < maximum_profiles, "This node already has 4096 own profiles.");
                    AssemblyProfile draft;
                    do { draft.id = make_stable_id(); } while (collection_ptr->reserved_ids.contains(draft.id));
                    collection_ptr->reserved_ids.insert(draft.id);
                    draft.height_m = 1;
                    if (auto accepted = edit_assembly_profile_geometry(owner, draft, length_unit == Unit::metre)) {
                        require(accepted->id == draft.id && accepted->material_slot == draft.material_slot,
                            "The outline editor changed the new profile identity or material slot.");
                        append_profile(*collection_ptr, page_ptr->sources[collection_ptr->source_index], std::move(*accepted), *page_ptr);
                        collection_ptr->table->setCurrentCell(collection_ptr->table->rowCount() - 1, 0);
                        update_geometry_controls(*page_ptr);
                        error->clear(); error->hide();
                    }
                } catch (const std::exception& failure) { error->setText(q(failure.what())); error->show(); }
            });
            QObject::connect(collection->remove, &QPushButton::clicked, owner, [this, page_ptr = &page, collection_ptr] {
                try {
                    require(!collection_ptr->initialize_profile_topology,
                        "Apply pending edge identities before removing profiles.");
                    const auto row = collection_ptr->table->currentRow();
                    require(row >= 0 && static_cast<std::size_t>(row) < collection_ptr->profiles.size() &&
                        !collection_ptr->table->selectedItems().empty(), "Select an own profile to remove.");
                    collection_ptr->profiles.erase(collection_ptr->profiles.begin() + row);
                    collection_ptr->table->removeRow(row);
                    update_geometry_controls(*page_ptr);
                    error->clear(); error->hide();
                } catch (const std::exception& failure) { error->setText(q(failure.what())); error->show(); }
            });
            page.profile_stack->addWidget(collection->widget);
            page.profile_collections[index] = std::move(collection);
        }
        page.profile_stack->setCurrentWidget(page.profile_collections[index]->widget);
        update_geometry_controls(page);
    }
    void build_geometry(Page& page, QTabWidget* tabs) {
        auto* form = tab(tabs, "Geometry");
        page.geometry_operation = operations(tabs, "assemblyGeometry");
        page.geometry_operation->setItemText(2, "Use type geometry");
        page.geometry_operation->setToolTip("Keep retains each node's own geometry. Set stores a local profile override, "
            "including when all dimension fields are blank. Use type geometry removes only this node's profile override.");
        form->addRow("Geometry operation", page.geometry_operation);
        auto* note = new QLabel("Dimensions measure this node's own outlines and extrusion, along its local axes, "
            "with root and part pose scales included. Entered dimensions also account for pose changes on other part pages. "
            "Child parts have their own Geometry pages. "
            "Blank dimensions retain each source; resizing anchors the local minimum X, Y and elevation. "
            "Current values describe the captured geometry and pose.", tabs);
        note->setWordWrap(true); form->addRow(note);
        const std::array<QString, 3> labels{"Width", "Depth", "Height"};
        for (std::size_t i = 0; i < page.dimensions.size(); ++i) {
            QString hint;
            std::optional<double> first;
            bool mixed = false, missing = false;
            for (const auto& selected : page.sources) {
                if (selected.profiles->empty()) { missing = true; continue; }
                const auto span = profile_spans(*selected.profiles, selected.node->transform)[i];
                if (!first) first = span;
                else if (*first != span) mixed = true;
            }
            hint = !first ? "No own profiles" : missing ? "Some nodes have no own profiles" :
                mixed ? "Multiple values" : length(*first);
            auto* input = field(tabs, "assemblyGeometry" + labels[i], hint);
            input->setAccessibleName("Geometry " + labels[i]);
            input->setEnabled(false);
            input->setToolTip("Current: " + hint + "\nEnter a positive physical length; blank retains each node's size. "
                "Each selected node is resized from its own profiles. You can review affected dimensions before applying. " +
                (length_unit == Unit::metre ? QString("Bare numbers use metres.") : QString("Bare numbers use feet.")) +
                "\nCircular arcs require width and depth to scale equally. Height includes elevation gaps between profiles. "
                "Dimensions resize the complete profile draft after any detailed edits.");
            page.dimensions[i] = input;
            QObject::connect(input, &QLineEdit::textChanged, owner, [this, page_ptr = &page] { update_geometry_controls(*page_ptr); });
            form->addRow(labels[i], input);
        }
        auto* local_note = new QLabel("Own profiles belong to the chosen component; nested parts retain their own geometry. "
            "Adding, removing or editing a profile changes only this component's local draft. "
            "Local elevation and extrusion height are measured before pose scaling. "
            "Edit outline and cutouts opens the vertex and circular-arc editor. "
            "Keep and Use type geometry ignore these drafts; Set applies them together with the common dimensions above.", tabs);
        local_note->setWordWrap(true); form->addRow(local_note);
        page.profile_stack = new QStackedWidget(tabs);
        page.profile_stack->setObjectName("assemblyGeometryProfileCollections");
        page.profile_collections.resize(page.sources.size());
        if (page.sources.size() > 1) {
            page.profile_source = new QComboBox(tabs);
            page.profile_source->setObjectName("assemblyGeometryProfileSource");
            page.profile_source->setAccessibleName("Component whose own profiles are edited");
            page.profile_source->setToolTip("Each selected component retains its own profile draft when you switch. "
                "Common dimensions resize every source separately; detailed profiles are never copied to other selected components.");
            for (std::size_t index = 0; index < page.sources.size(); ++index) {
                const auto& selected = page.sources[index];
                auto label = selected.type->name.empty() ? QString("Component") : q(selected.type->name);
                if (selected.target.document_entity_id) {
                    const auto& properties = source.entities().at(*selected.target.document_entity_id).properties;
                    if (properties.is_object() && properties.contains("name") && properties.at("name").is_string()) {
                        const auto name = q(properties.at("name").get<std::string>()).trimmed();
                        if (!name.isEmpty()) label = name + (selected.target.part_path.empty() ? QString{} : " / " + label);
                    }
                }
                page.profile_source->addItem(label + QString(" (%1)").arg(static_cast<qulonglong>(index + 1)));
                page.profile_source->setItemData(static_cast<int>(index), q(selected.target.catalog_id) + " / " +
                    q(selected.target.instance_id) + (selected.target.document_entity_id ?
                        " / " + q(*selected.target.document_entity_id) : QString{}) +
                    (selected.target.part_path.empty() ? " / root" : " / " + path_label(selected.target.part_path)), Qt::ToolTipRole);
            }
            form->addRow("Own profiles of", page.profile_source);
            QObject::connect(page.profile_source, &QComboBox::currentIndexChanged, owner, [this, page_ptr = &page](int index) {
                if (index < 0) return;
                try {
                    show_profile_collection(*page_ptr, static_cast<std::size_t>(index));
                    error->clear(); error->hide();
                } catch (const std::exception& failure) { error->setText(q(failure.what())); error->show(); }
            });
        }
        show_profile_collection(page, 0);
        form->addRow(page.profile_stack);
        QObject::connect(page.geometry_operation, &QComboBox::currentIndexChanged, owner, [this, page_ptr = &page](int) {
            update_geometry_controls(*page_ptr);
        });
    }
    void build(Page& page) {
        auto* tabs = new QTabWidget(stack);
        build_geometry(page, tabs);
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

    std::array<std::optional<double>, 3> read_dimensions(const Page& page) const {
        std::array<std::optional<double>, 3> dimensions;
        if (operation(page.geometry_operation) != Operation::set) return dimensions;
        for (std::size_t i = 0; i < dimensions.size(); ++i) if (supplied(page.dimensions[i])) {
            const auto value = parse_quantity(page.dimensions[i]->text().toStdString(), length_unit).metres;
            require(std::isfinite(value) && value > 0,
                page.dimensions[i]->accessibleName().toStdString() + " must be finite and positive.");
            dimensions[i] = value;
        }
        return dimensions;
    }
    AssemblyTransform final_geometry_frame(const Target& target, const std::map<Target, Intent>& intents) const {
        auto path = target;
        path.part_path.clear();
        AssemblyTransform frame;
        // Only root and exact stable-path ancestors of this node contribute.
        // This detached scale calculation supplies no candidate/document authority.
        for (std::size_t depth = 0; depth <= target.part_path.size(); ++depth) {
            const auto selected = resolve(path);
            auto local = selected.pose;
            if (const auto found = intents.find(path); found != intents.end()) {
                const auto& patch = found->second.pose;
                if (patch.operation == Operation::clear) {
                    require(!path.part_path.empty() && selected.node->source_part.has_value(),
                        "Only a nested part pose can be cleared.");
                    local = selected.node->source_part->transform;
                } else if (patch.operation == Operation::set) {
                    local.scale = patch.scale.value_or(local.scale);
                    local.vertical_scale = patch.vertical_scale.value_or(local.vertical_scale);
                }
            }
            // Yaw, reflection and translation do not change spans along the
            // selected local axes. Reuse the model's checked scale composition.
            frame = compose_assembly_transform(frame, AssemblyTransform{{}, 0, local.scale, false, local.vertical_scale});
            if (depth < target.part_path.size()) path.part_path.push_back(target.part_path[depth]);
        }
        return frame;
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
        const auto geometry_operation = operation(page.geometry_operation);
        std::vector<Intent> result;
        for (std::size_t index = 0; index < page.sources.size(); ++index) {
            const auto& selected = page.sources[index];
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
            intent.profile_geometry.operation = geometry_operation;
            const auto* collection = page.profile_collections[index].get();
            if (collection && collection->initialize_profile_topology)
                require(geometry_operation == Operation::set, "Apply pending edge identities with Set geometry before making other geometry changes.");
            if (geometry_operation == Operation::set) {
                // Dimensions are applied only after all pages' pose patches have
                // been collected. Each cohort member keeps its actual profiles.
                intent.profile_geometry.value = collection ? profile_draft(*collection) : *selected.profiles;
                if (collection && collection->initialize_profile_topology) {
                    const auto& proposed = *intent.profile_geometry.value;
                    require(proposed.size() == selected.profiles->size(), "Pending edge identities must retain the captured own-profile roster.");
                    for (std::size_t i = 0; i < proposed.size(); ++i) {
                        auto expected = (*selected.profiles)[i];
                        if (!expected.topology) expected.topology = collection->profiles[i].draft.topology;
                        require(expected.topology.has_value() && proposed[i] == expected,
                            "Apply pending edge identities before changing profile geometry, extrusion or material slots.");
                    }
                    intent.initialize_profile_topology = true;
                }
            }
            result.push_back(std::move(intent));
        }
        return result;
    }
    std::vector<Intent> intents() const {
        std::map<Target, Intent> result;
        std::map<Target, std::array<std::optional<double>, 3>> dimensions;
        for (const auto& target : original) { Intent retained; retained.target = target; result.emplace(target, std::move(retained)); }
        for (std::size_t index = 0; index < pages.size(); ++index) {
            if (!pages[index]->widget) continue;
            auto values = read(*pages[index]);
            const auto requested_dimensions = read_dimensions(*pages[index]);
            if (index != 0 && std::none_of(values.begin(), values.end(), changed)) continue;
            for (auto& intent : values) {
                const auto found = result.find(intent.target);
                if (found != result.end() && changed(found->second)) {
                    require(!changed(intent), "A source part was edited on more than one page. Keep its edits on one page.");
                    continue;
                }
                const auto target = intent.target;
                result.insert_or_assign(target, std::move(intent));
                dimensions.insert_or_assign(target, requested_dimensions);
                require(result.size() <= maximum_targets, "The combined selection and edited parts exceed 1000 targets.");
            }
        }
        std::vector<Intent> output;
        output.reserve(result.size());
        for (auto& [target, intent] : result) {
            const auto selected = resolve(target);
            if (intent.profile_geometry.operation == Operation::clear)
                intent.review_profile_topology = identified(*selected.profiles) || identified(selected.type->profiles);
            if (intent.profile_geometry.operation == Operation::set) {
                require(intent.profile_geometry.value.has_value(), "Set geometry requires actual profile data.");
                auto& profiles = *intent.profile_geometry.value;
                try {
                    const auto& requested = dimensions.at(target);
                    const bool resize = std::any_of(requested.begin(), requested.end(), [](const auto& value) { return value.has_value(); });
                    require(!intent.initialize_profile_topology || !resize,
                        "Apply pending edge identities before resizing geometry.");
                    if (resize)
                        resize_profiles(profiles, final_geometry_frame(target, result), requested);
                    if (!intent.initialize_profile_topology && identified(*selected.profiles)) {
                        // Classify the complete final draft after physical resizing.
                        // Fresh IDs exist only in this collected intent; editing
                        // or recollecting a draft cannot reuse a stale review map.
                        collect_reviewed_topology(profiles, selected);
                        intent.review_profile_topology = true;
                    }
                    (void)validate_assembly_profiles(profiles);
                } catch (const std::exception& failure) {
                    throw std::invalid_argument("Geometry for " + target.instance_id +
                        (target.part_path.empty() ? std::string(" (root)") : " / " + path_label(target.part_path).toStdString()) +
                        ": " + failure.what());
                }
            }
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
