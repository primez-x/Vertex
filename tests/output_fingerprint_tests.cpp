#include "sketch/document.hpp"
#include "sketch/output_fingerprint.hpp"
#include "support/noninteractive_errors.hpp"

#include <iostream>
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using sketch::FingerprintDependencyGroup;
using sketch::FingerprintGroupState;
using sketch::FingerprintResource;
using sketch::FingerprintRole;
using sketch::OutputFingerprintInputs;
using Json = nlohmann::json;

void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

std::vector<std::byte> bytes_for(std::string_view tag) {
    std::vector<std::byte> bytes;
    bytes.reserve(tag.size());
    for (const auto value : tag) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    return bytes;
}

std::string hash_for(std::string_view tag) {
    const auto bytes = bytes_for(tag);
    return sketch::sha256_hex(std::span<const std::byte>(bytes.data(), bytes.size()));
}

std::string invalid_utf8() {
    std::string value;
    value.push_back(static_cast<char>(0xc3));
    value.push_back(static_cast<char>(0x28));
    return value;
}

void refresh_digest(Json& encoded) {
    encoded.at("digest_sha256") = hash_for(encoded.at("manifest").dump());
}

FingerprintResource resource(std::string id, std::string tag) {
    return FingerprintResource{std::move(id), hash_for(tag), Json{{"tag", std::move(tag)}}};
}

FingerprintDependencyGroup resources(std::initializer_list<FingerprintResource> values) {
    FingerprintDependencyGroup group;
    group.state = FingerprintGroupState::resources;
    group.resources.assign(values.begin(), values.end());
    return group;
}

FingerprintRole role_resource(std::string id, std::string tag) {
    FingerprintRole role;
    role.state = FingerprintGroupState::resources;
    role.resources.push_back(resource(std::move(id), std::move(tag)));
    return role;
}

OutputFingerprintInputs valid_inputs() {
    OutputFingerprintInputs inputs;
    inputs.profiles = resources({resource("profile-b", "profile-b"), resource("profile-a", "profile-a")});
    inputs.fonts = resources({resource("font-inter", "font-inter")});
    inputs.views = resources({resource("view-default", "view-default")});
    inputs.crs = resources({resource("crs-local", "crs-local")});
    inputs.processing_components.state = FingerprintGroupState::resources;
    inputs.processing_components.roles.emplace("kernel", role_resource("kernel-occt", "kernel"));
    inputs.processing_components.roles.emplace("solver", role_resource("solver-planegcs", "solver"));
    inputs.processing_components.roles.emplace("renderer", role_resource("renderer-occt", "renderer"));
    inputs.processing_components.roles.emplace("adapters", role_resource("adapter-native", "adapter"));
    inputs.application_build = resources({resource("application-build", "build-2026-09-09")});
    return inputs;
}

sketch::Document document_with_asset(
    std::string asset_tag = "raw-private-asset-sentinel") {
    const sketch::Entity entity{
        "wall-1", "wall",
        Json{{"baseline", Json{{"end", Json{4.0, 0.0}}, {"start", Json{0.0, 0.0}}}},
             {"height_m", 2.4},
             {"thickness_m", 0.2},
             {"private_entity_sentinel", "private-entity-marker"}},
        true,
        Json{{"source", "test"}}};
    auto asset = sketch::Asset::create("asset-1", "application/octet-stream",
                                       bytes_for(asset_tag), Json{{"origin", "test"}});
    return sketch::Document::create({entity}, {std::move(asset)});
}

void test_deterministic_and_sorted() {
    auto document = document_with_asset();
    const auto inputs = valid_inputs();
    const auto first = sketch::make_output_fingerprint(document.snapshot(), inputs);
    const auto second = sketch::make_output_fingerprint(document.snapshot(), inputs);
    check(first.digest_sha256 == second.digest_sha256,
          "identical snapshot and inputs must produce the same digest");
    check(first.manifest.dump() == second.manifest.dump(),
          "identical snapshot and inputs must produce the same canonical manifest");

    auto reordered = inputs;
    std::reverse(reordered.profiles.resources.begin(), reordered.profiles.resources.end());
    const auto reordered_fp = sketch::make_output_fingerprint(document.snapshot(), reordered);
    check(reordered_fp.digest_sha256 == first.digest_sha256,
          "dependency resource order must not affect the fingerprint");

    const auto encoded = sketch::serialize_output_fingerprint(first);
    std::string error;
    const auto decoded = sketch::deserialize_output_fingerprint(encoded, &error);
    check(decoded.has_value() && error.empty(), "a serialized fingerprint must round-trip");
    check(decoded->digest_sha256 == first.digest_sha256 &&
              decoded->manifest.dump() == first.manifest.dump(),
          "round-trip fingerprint must preserve canonical content");
}

void test_each_group_stales_output() {
    auto document = document_with_asset();
    const auto baseline = valid_inputs();
    const auto fingerprint = sketch::make_output_fingerprint(document.snapshot(), baseline);

    const std::vector<std::string> expected_groups{
        "profiles", "fonts", "views", "crs", "processing_components", "application_build"};
    for (const auto& group_name : expected_groups) {
        auto changed = baseline;
        if (group_name == "processing_components") {
            changed.processing_components.roles.at("solver").resources.at(0).metadata["changed"] =
                true;
        } else if (group_name == "application_build") {
            changed.application_build.resources.at(0).metadata["changed"] = true;
        } else if (group_name == "profiles") {
            changed.profiles.resources.at(0).metadata["changed"] = true;
        } else if (group_name == "fonts") {
            changed.fonts.resources.at(0).metadata["changed"] = true;
        } else if (group_name == "views") {
            changed.views.resources.at(0).metadata["changed"] = true;
        } else {
            changed.crs.resources.at(0).metadata["changed"] = true;
        }
        const auto status = sketch::check_output_fingerprint_current(
            fingerprint, document.snapshot(), changed);
        check(status.valid && !status.current, "changing a dependency must make output stale");
        check(status.changed_groups.size() == 1 && status.changed_groups.front() == group_name,
              "currentness must identify the changed dependency group");
    }
}

void test_document_and_asset_changes() {
    auto document = document_with_asset();
    const auto inputs = valid_inputs();
    const auto fingerprint = sketch::make_output_fingerprint(document.snapshot(), inputs);

    auto same_revision_snapshot = document.snapshot();
    check(same_revision_snapshot.revision() == document.revision(),
          "semantic comparison fixture must keep the document revision unchanged");
    auto& same_revision_entity =
        const_cast<sketch::Entity&>(same_revision_snapshot.entities().at("wall-1"));
    same_revision_entity.properties["baseline"]["end"][0] = 6.0;
    const auto same_revision_status = sketch::check_output_fingerprint_current(
        fingerprint, same_revision_snapshot, inputs);
    check(same_revision_status.valid && !same_revision_status.current &&
              same_revision_status.changed_groups == std::vector<std::string>{"document"},
          "different semantics at the same revision must stale the document group");

    const auto changed_entity = sketch::Entity{"wall-1", "wall",
                                                Json{{"baseline", Json{{"end", Json{5.0, 0.0}},
                                                                           {"start", Json{0.0, 0.0}}}},
                                                     {"height_m", 2.4}, {"thickness_m", 0.2}},
                                                true, Json{{"source", "test"}}};
    document.apply(sketch::ApplyEntityChanges{
        document.revision(), {sketch::EntityChange::upsert(changed_entity)}, {}, "edit wall"});
    const auto entity_status = sketch::check_output_fingerprint_current(
        fingerprint, document.snapshot(), inputs);
    check(entity_status.valid && !entity_status.current &&
              entity_status.changed_groups == std::vector<std::string>{"document"},
          "changing entity semantics must stale the document group");

    auto asset_document = document_with_asset();
    const auto asset_fingerprint = sketch::make_output_fingerprint(asset_document.snapshot(), inputs);
    const auto changed_asset = sketch::Asset::create(
        "asset-1", "application/octet-stream", bytes_for("asset-b"), Json{{"origin", "test"}});
    asset_document.apply(sketch::ApplyEntityChanges{
        asset_document.revision(), {}, {sketch::AssetChange::upsert(changed_asset)}, "edit asset"});
    const auto asset_status = sketch::check_output_fingerprint_current(
        asset_fingerprint, asset_document.snapshot(), inputs);
    check(asset_status.valid && !asset_status.current &&
              asset_status.changed_groups == std::vector<std::string>{"document"},
          "changing asset bytes must stale the document group");
}

void test_explicit_empty_states_and_required_roles() {
    auto document = document_with_asset();
    auto inputs = valid_inputs();
    inputs.fonts.state = FingerprintGroupState::no_resource;
    inputs.fonts.resources.clear();
    inputs.fonts.reason = "output contains no text";
    const auto fingerprint = sketch::make_output_fingerprint(document.snapshot(), inputs);
    check(fingerprint.manifest.at("dependencies").at("fonts").at("state") == "no_resource",
          "no-resource groups must be represented explicitly");

    auto missing_role = valid_inputs();
    missing_role.processing_components.roles.erase("solver");
    bool rejected = false;
    try {
        (void)sketch::make_output_fingerprint(document.snapshot(), missing_role);
    } catch (const sketch::OutputFingerprintError& error) {
        rejected = error.code() == sketch::OutputFingerprintErrorCode::invalid_dependency;
    }
    check(rejected, "processing roles cannot be silently omitted");

    auto no_build = valid_inputs();
    no_build.application_build.state = FingerprintGroupState::not_applicable;
    no_build.application_build.resources.clear();
    no_build.application_build.reason = "missing build metadata";
    rejected = false;
    try {
        (void)sketch::make_output_fingerprint(document.snapshot(), no_build);
    } catch (const sketch::OutputFingerprintError& error) {
        rejected = error.code() == sketch::OutputFingerprintErrorCode::invalid_dependency;
    }
    check(rejected, "application build identity is mandatory");
}

void test_invalid_hashes_and_tampering() {
    auto document = document_with_asset();
    auto inputs = valid_inputs();
    inputs.views.resources.at(0).sha256 = "bad";
    bool rejected = false;
    try {
        (void)sketch::make_output_fingerprint(document.snapshot(), inputs);
    } catch (const sketch::OutputFingerprintError& error) {
        rejected = error.code() == sketch::OutputFingerprintErrorCode::invalid_dependency;
    }
    check(rejected, "dependency hashes must be valid SHA-256 values");

    auto nonfinite = valid_inputs();
    nonfinite.views.resources.at(0).metadata["nan"] =
        std::numeric_limits<double>::quiet_NaN();
    rejected = false;
    try {
        (void)sketch::make_output_fingerprint(document.snapshot(), nonfinite);
    } catch (const sketch::OutputFingerprintError& error) {
        rejected = error.code() == sketch::OutputFingerprintErrorCode::invalid_dependency;
    }
    check(rejected, "non-finite dependency metadata must be rejected");

    auto invalid_document_snapshot = document.snapshot();
    auto& invalid_document_entity =
        const_cast<sketch::Entity&>(invalid_document_snapshot.entities().at("wall-1"));
    invalid_document_entity.properties["invalid_utf8"] = invalid_utf8();
    rejected = false;
    try {
        (void)sketch::make_output_fingerprint(invalid_document_snapshot, valid_inputs());
    } catch (const sketch::OutputFingerprintError& error) {
        rejected = error.code() == sketch::OutputFingerprintErrorCode::invalid_document;
    } catch (const std::exception&) {
    }
    check(rejected, "invalid document UTF-8 must produce a typed document error");

    auto invalid_dependency = valid_inputs();
    invalid_dependency.views.resources.at(0).metadata["invalid_utf8"] = invalid_utf8();
    rejected = false;
    try {
        (void)sketch::make_output_fingerprint(document.snapshot(), invalid_dependency);
    } catch (const sketch::OutputFingerprintError& error) {
        rejected = error.code() == sketch::OutputFingerprintErrorCode::invalid_dependency;
    } catch (const std::exception&) {
    }
    check(rejected, "invalid dependency UTF-8 must produce a typed dependency error");

    auto malformed_snapshot = document.snapshot();
    auto& malformed_asset =
        const_cast<sketch::Asset&>(malformed_snapshot.assets().at("asset-1"));
    malformed_asset.sha256 = std::string(64, '0');
    rejected = false;
    try {
        (void)sketch::make_output_fingerprint(malformed_snapshot, valid_inputs());
    } catch (const sketch::OutputFingerprintError& error) {
        rejected = error.code() == sketch::OutputFingerprintErrorCode::invalid_asset;
    }
    check(rejected, "asset SHA-256 metadata must match freshly hashed bytes");

    const auto valid = sketch::make_output_fingerprint(document.snapshot(), valid_inputs());
    auto encoded = sketch::serialize_output_fingerprint(valid);
    const auto serialized_text = encoded.dump();
    check(serialized_text.find("\"bytes_hex\"") == std::string::npos &&
              serialized_text.find("\"entities\"") == std::string::npos &&
              serialized_text.find("private-entity-marker") == std::string::npos &&
              serialized_text.find("raw-private-asset-sentinel") == std::string::npos,
          "serialized fingerprints must not duplicate entity data or raw asset bytes");

    auto invalid_manifest = valid;
    invalid_manifest.manifest.at("document").at("document_id") = invalid_utf8();
    rejected = false;
    try {
        (void)sketch::serialize_output_fingerprint(invalid_manifest);
    } catch (const sketch::OutputFingerprintError& error) {
        rejected = error.code() == sketch::OutputFingerprintErrorCode::invalid_manifest;
    } catch (const std::exception&) {
    }
    check(rejected, "invalid serialized UTF-8 must produce a typed manifest error");

    auto reversed_resources = encoded;
    auto& profile_resources =
        reversed_resources.at("manifest").at("dependencies").at("profiles").at("resources");
    std::reverse(profile_resources.begin(), profile_resources.end());
    refresh_digest(reversed_resources);
    std::string error;
    check(!sketch::deserialize_output_fingerprint(reversed_resources, &error).has_value() &&
              error.find("sorted") != std::string::npos,
          "resource arrays must remain in canonical ID order after digest recomputation");

    for (const auto* field_name : {"asset_count", "entity_count", "revision"}) {
        auto negative_value = encoded;
        negative_value.at("manifest").at("document")[field_name] = -1;
        refresh_digest(negative_value);
        error.clear();
        check(!sketch::deserialize_output_fingerprint(negative_value, &error).has_value() &&
                  !error.empty(),
              "negative document counters and revisions must be rejected");

        auto maximum_value = encoded;
        maximum_value.at("manifest").at("document")[field_name] =
            std::numeric_limits<std::uint64_t>::max();
        refresh_digest(maximum_value);
        error.clear();
        check(sketch::deserialize_output_fingerprint(maximum_value, &error).has_value() &&
                  error.empty(),
              "unsigned 64-bit document counters and revisions must be accepted");
    }

    auto incomplete_manifest = encoded;
    incomplete_manifest.at("manifest").at("document").erase("head_content_sha256");
    refresh_digest(incomplete_manifest);
    check(!sketch::deserialize_output_fingerprint(incomplete_manifest, &error).has_value() &&
              !error.empty(),
          "incomplete document summaries must fail manifest validation");

    auto incomplete_processing = encoded;
    incomplete_processing.at("manifest")
        .at("dependencies")
        .at("processing_components")
        .at("roles")
        .erase("solver");
    refresh_digest(incomplete_processing);
    error.clear();
    check(!sketch::deserialize_output_fingerprint(incomplete_processing, &error).has_value() &&
              !error.empty(),
          "incomplete processing role coverage must fail manifest validation");

    auto changed_digest = sketch::serialize_output_fingerprint(valid);
    changed_digest.at("digest_sha256") = std::string(64, '0');
    error.clear();
    check(!sketch::deserialize_output_fingerprint(changed_digest, &error).has_value() &&
              error.find("digest") != std::string::npos,
          "mutated manifest digest must be rejected");
}

}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        test_deterministic_and_sorted();
        test_each_group_stales_output();
        test_document_and_asset_changes();
        test_explicit_empty_states_and_required_roles();
        test_invalid_hashes_and_tampering();
        std::cout << "Output fingerprint tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
