#include "assembly_directional_resize_authoring.hpp"
#include "assembly_profile_topology_authoring.hpp"

#include <QByteArray>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch::desktop {
namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

void require_budget(const AssemblyExpansionBudget& budget) {
    require(budget.max_nodes <= 4096 && budget.max_profile_segments <= 262144 &&
        budget.max_mapped_profile_work <= 67108864 &&
        budget.consumed_nodes <= budget.max_nodes &&
        budget.consumed_profile_segments <= budget.max_profile_segments &&
        budget.consumed_mapped_profile_work <= budget.max_mapped_profile_work,
        "invalid component directional resize work budget");
}

// The core composes through each source map retaining its source origin.
// With that exact anchor, equal coefficients/target mean the same operator;
// no rounded point sampling or unbounded rational work is necessary here.
bool genuine_drafted_map_change(const AssemblyProfile& source, const AssemblyProfile& candidate) {
    require(candidate.planar_affine.has_value(), "directional resize draft has no candidate map");
    const auto& after = *candidate.planar_affine;
    if (source.planar_affine) {
        const auto& before = *source.planar_affine;
        require(before.source_origin.x == after.source_origin.x &&
            before.source_origin.y == after.source_origin.y,
            "directional resize draft changed its source map anchor");
        return before.xx != after.xx || before.xy != after.xy ||
            before.yx != after.yx || before.yy != after.yy ||
            before.target_origin.x != after.target_origin.x ||
            before.target_origin.y != after.target_origin.y;
    }
    return after.xx != 1.0 || after.xy != 0.0 || after.yx != 0.0 || after.yy != 1.0 ||
        after.source_origin.x != after.target_origin.x || after.source_origin.y != after.target_origin.y;
}

std::vector<AssemblyInstancePropertyEditIntent> draft_resize_intents(
    const DocumentSnapshot& source, std::span<const AssemblyDirectionalResizeTarget> targets,
    AssemblyExpansionBudget& available_work,
    bool require_world_axes) {
    require_budget(available_work);
    require(source.is_editable(), "component directional resize source is read only");
    require(!targets.empty() && targets.size() <= 1000,
        "component directional resize requires 1..1000 independent document entities");
    for (const auto& target : targets) {
        require(!target.independent_entity_id.empty() && target.independent_entity_id.size() <= 128,
            "component directional resize independent entity ID exceeds the document identity bound");
        const auto& operation = target.operation;
        require(std::isfinite(operation.world_anchor_m.x) && std::isfinite(operation.world_anchor_m.y) &&
            std::isfinite(operation.x_factor) && operation.x_factor > 0 &&
            std::isfinite(operation.y_factor) && operation.y_factor > 0,
            "component directional resize anchor/factors must be finite and positive");
        require(!operation.world_axis_rotation_radians || std::isfinite(*operation.world_axis_rotation_radians),
            "component directional resize world axis rotation must be finite");
        require(!require_world_axes || operation.world_axis_rotation_radians.has_value(),
            "component directional resize cohort requires explicit world axes");
        require(operation.x_factor == targets.front().operation.x_factor &&
            operation.y_factor == targets.front().operation.y_factor,
            "component directional resize cohort requires identical directional factors");
    }
    std::vector<AssemblyDirectionalResizeTarget> sorted_targets(targets.begin(), targets.end());
    std::sort(sorted_targets.begin(), sorted_targets.end(), [](const auto& first, const auto& second) {
        return first.independent_entity_id < second.independent_entity_id;
    });
    require(std::adjacent_find(sorted_targets.begin(), sorted_targets.end(), [](const auto& first, const auto& second) {
        return first.independent_entity_id == second.independent_entity_id;
    }) == sorted_targets.end(),
        "component directional resize independent entity IDs must be nonempty and unique");

    auto pending = available_work;
    // Raw catalogue work has different units from mapped expansion work.
    // Admit all distinct actual catalogues together BEFORE any model decode.
    // Never reset or raise either ledger to admit another root/candidate.
    AssemblyCatalogTransferBudget catalog_work;
    std::map<std::string, const Entity*, std::less<>> catalogs;
    std::vector<std::pair<AssemblyDocumentInstance, AssemblyDirectionalResize>> documents;
    documents.reserve(sorted_targets.size());
    for (const auto& target : sorted_targets) {
        const auto& independent_entity_id = target.independent_entity_id;
        const auto entity = source.entities().find(independent_entity_id);
        require(entity != source.entities().end() && entity->second.id == independent_entity_id &&
            entity->second.type == "assembly_instance",
            "component directional resize requires an actual independent document entity");
        auto document = decode_document_assembly_instance(entity->second, pending);
        require(document.instance.id == independent_entity_id && document.instance.root_transform &&
            !document.instance.placement,
            "component directional resize independent source identity/pose is inconsistent");
        const auto catalog = source.entities().find(document.assembly_catalog_id);
        require(catalog != source.entities().end() && catalog->second.id == document.assembly_catalog_id &&
            catalog->second.type == "assembly_model" && catalog->second.properties.is_object() &&
            catalog->second.properties.contains("model") && catalog->second.properties.at("model").is_object(),
            "component directional resize actual catalogue is missing or inconsistent");
        if (catalogs.emplace(document.assembly_catalog_id, &catalog->second).second)
            admit_existing_assembly_catalog_work(catalog->second, catalog_work);
        documents.emplace_back(std::move(document), target.operation);
    }
    std::map<std::string, AssemblyModel, std::less<>> models;
    for (const auto& [catalog_id, catalog] : catalogs)
        models.emplace(catalog_id, AssemblyModel::from_json(catalog->properties.at("model")));

    // Capture one namespace once. The sequence is deterministic in sorted
    // root/path order and actual profile/ring order, including collision retry.
    // A unit/no-change operation never allocates topology identities.
    QUuid proposal_namespace;
    std::uint64_t next_identity = 0;
    std::set<std::string> issued;
    const AssemblyProfileTopologyIdentityGenerator identity = [&] {
        std::string id;
        do {
            require(next_identity != std::numeric_limits<std::uint64_t>::max(),
                "component directional resize topology identity sequence exhausted");
            id = QUuid::createUuidV5(proposal_namespace,
                QByteArray::number(static_cast<qulonglong>(++next_identity)))
                .toString(QUuid::WithoutBraces).toStdString();
        } while (!issued.insert(id).second);
        return id;
    };

    std::vector<AssemblyInstancePropertyEditIntent> intents;
    for (const auto& [document, operation] : documents) {
        const auto& model = models.at(document.assembly_catalog_id);
        const auto actual = model.expand(document.instance, pending);
        require(actual.source_instance == document.instance,
            "component directional resize actual root differs from the captured source");
        const auto draft = draft_assembly_directional_resize(model, actual, operation, pending);
        require(draft.source_instance == document.instance,
            "component directional resize draft differs from the captured source");

        std::vector<const AssemblyDirectionalResizeNodeDraft*> changed;
        for (const auto& node : draft.nodes) {
            // Inspect ALL nodes. An empty own roster never prunes descendants.
            if (node.source_profiles == node.candidate_profiles) continue;
            require(intents.size() + changed.size() < 1000,
                "component directional resize exceeds 1000 changed nodes; the complete operation is refused");
            changed.push_back(&node);
        }
        std::sort(changed.begin(), changed.end(), [](const auto* first, const auto* second) {
            return first->part_path < second->part_path;
        });
        require(std::adjacent_find(changed.begin(), changed.end(), [](const auto* first, const auto* second) {
            return first->part_path == second->part_path;
        }) == changed.end(), "component directional resize has duplicate actual part paths");
        require(draft.changed == !changed.empty(), "component directional resize changed roster is inconsistent");

        std::map<std::string, const AssemblyType*, std::less<>> types;
        for (const auto& type : model.types()) types.emplace(type.id, &type);
        if (!changed.empty() && proposal_namespace.isNull()) proposal_namespace = QUuid::createUuid();
        for (const auto* node : changed) {
            const auto type = types.find(node->type_id);
            require(type != types.end(), "component directional resize actual node type is missing");
            require(node->source_profiles.size() == node->candidate_profiles.size(),
                "component directional resize changed its actual own-profile roster");
            bool deformed = false;
            for (std::size_t i = 0; i < node->source_profiles.size(); ++i) {
                const auto& before = node->source_profiles[i];
                const auto& after = node->candidate_profiles[i];
                require(before.id == after.id, "component directional resize changed an actual profile identity");
                if (before.planar_affine == after.planar_affine) continue;
                require(genuine_drafted_map_change(before, after),
                    "component directional resize changed only a map representation");
                deformed = true;
            }
            require(deformed, "component directional resize changed a roster without genuine map deformation");

            AssemblyInstancePropertyEditIntent intent;
            intent.target = {document.assembly_catalog_id, document.instance.id,
                document.instance.id, node->part_path};
            intent.profile_geometry.operation = AssemblyInstancePropertyPatchOperation::set;
            intent.profile_geometry.value = node->candidate_profiles;
            collect_reviewed_profile_topology(*intent.profile_geometry.value,
                node->source_profiles, type->second->profiles, identity);
            if (assembly_profiles_have_topology(node->source_profiles))
                intent.review_profile_topology = true;
            else
                intent.initialize_deformed_profile_topology = true;
            intents.push_back(std::move(intent));
        }
    }
    // Model/draft admission already checked all actual/candidate geometry in
    // the shared ledger. Identity assignment adds only structurally validated
    // topology; enclosing reviewed preparation owns codec/native admission.
    // Qualified targets sort by catalog before instance/path. Root-ID order
    // alone is not canonical when a cohort spans differently named catalogs.
    std::sort(intents.begin(), intents.end(), [](const auto& first, const auto& second) {
        return first.target < second.target;
    });
    available_work = std::move(pending);
    return intents;
}

std::vector<AssemblyInstancePropertyEditIntent> draft_common_resize_intents(
    const DocumentSnapshot& source, std::span<const std::string> independent_entity_ids,
    const AssemblyDirectionalResize& operation, AssemblyExpansionBudget& available_work,
    bool require_world_axes) {
    // Bound count and every identity before copying IDs into target/operator
    // pairs. The shared producer authenticates every pair and actual source.
    require(!independent_entity_ids.empty() && independent_entity_ids.size() <= 1000,
        "component directional resize requires 1..1000 independent document entities");
    for (const auto& id : independent_entity_ids)
        require(!id.empty() && id.size() <= 128,
            "component directional resize independent entity ID exceeds the document identity bound");
    std::vector<AssemblyDirectionalResizeTarget> targets;
    targets.reserve(independent_entity_ids.size());
    for (const auto& id : independent_entity_ids) targets.push_back({id, operation});
    return draft_resize_intents(source, targets, available_work, require_world_axes);
}

} // namespace

std::vector<AssemblyInstancePropertyEditIntent> draft_assembly_directional_resize_intents(
    const DocumentSnapshot& source, const std::string& independent_entity_id,
    const AssemblyDirectionalResize& operation, AssemblyExpansionBudget& available_work) {
    return draft_common_resize_intents(source, std::span<const std::string>(&independent_entity_id, 1),
        operation, available_work, false);
}

std::vector<AssemblyInstancePropertyEditIntent> draft_assembly_directional_resize_intents(
    const DocumentSnapshot& source, std::span<const std::string> independent_entity_ids,
    const AssemblyDirectionalResize& operation, AssemblyExpansionBudget& available_work) {
    return draft_common_resize_intents(source, independent_entity_ids, operation, available_work, true);
}

std::vector<AssemblyInstancePropertyEditIntent> draft_assembly_directional_resize_intents(
    const DocumentSnapshot& source, std::span<const AssemblyDirectionalResizeTarget> targets,
    AssemblyExpansionBudget& available_work) {
    return draft_resize_intents(source, targets, available_work, true);
}

} // namespace sketch::desktop
