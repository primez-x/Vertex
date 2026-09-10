#include "sketch/recovery_copy_record.hpp"
#include "support/noninteractive_errors.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}
template<class F> void rejected(F&& f) {
    try { f(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("expected invalid recovery copy rejection");
}
RecoveryCopyRecord fixture() {
    RecoveryCopyRecord value;
    value.archive_id = "archive"; value.owner_token = "owner"; value.document_id = "document";
    value.ownership = {{"claimed_lock", true}, {"future", Json::array({nullptr, 1, "x"})}};
    value.extensions = {{"nested", {{"preserved", false}}}};
    return value;
}
void round_trip(const RecoveryCopyRecord& value) {
    const auto wire = encode_recovery_copy_record(value);
    const auto result = decode_recovery_copy_record(wire);
    require(result.supported() && !result.opaque() && *result.record == value,
            "recovery copy exact typed round trip");
    require(encode_recovery_copy_record(*result.record) == wire, "stable wire round trip");
}
void check_round_trip_and_history() {
    auto value = fixture(); round_trip(value);
    const auto wire = encode_recovery_copy_record(value);
    require(wire.at("source_path").is_null() && wire.at("source_sha256").is_null() &&
            wire.at("explicitly_saved_document_revision").is_null(), "required nullable fields");
    require(wire.at("role") == "recovery_copy", "exact role");
    value.source_path = "../unopened/source"; round_trip(value);
    value.source_path.reset(); value.source_sha256 = std::string(64, 'a'); round_trip(value);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    value.explicitly_saved_document_revision = maximum;
    value.explicit_save_generation = value.workspace_epoch = value.edited_generation = maximum;
    value.checkpoint_generation = value.autosaved_checkpoint_generation = value.saved_edited_generation = maximum;
    value.source_path = std::string(131072, 'p'); value.source_sha256 = std::string(64, 'a');
    value.archive_id = value.owner_token = value.document_id = std::string(128, 'i');
    round_trip(value);
    value = fixture(); value.edited_generation = 20; value.saved_edited_generation = 7;
    value.explicit_save_generation = 4; value.checkpoint_generation = 3;
    round_trip(value); // These are independent domains, not one shared counter.
    auto document = Document::create();
    value.document_id = document.snapshot().document_id();
    auto snapshot = document.snapshot();
    validate_recovery_copy_document(value, snapshot);
    require(snapshot.dirty() && !snapshot.saved_revision_optional(), "null anchor leaves never-saved dirty state");
    auto restricted = boundary_authoring_default_resource_policy;
    restricted.max_encoded_bytes = 1;
    rejected([&] { validate_recovery_copy_document(value, snapshot, restricted); });
    document.mark_saved(document.revision());
    value.explicitly_saved_document_revision = document.revision();
    ApplyEntityChanges edit;
    edit.expected_revision = document.revision();
    edit.entity_changes.push_back(EntityChange::upsert(Entity::create("label", {{"text", "edit"}})));
    (void)document.apply(edit);
    snapshot = document.snapshot();
    const auto revision = snapshot.revision();
    const auto saved = snapshot.saved_revision_optional();
    validate_recovery_copy_document(value, snapshot);
    require(snapshot.dirty() && snapshot.revision() == revision && snapshot.saved_revision_optional() == saved &&
            value.explicitly_saved_document_revision != revision, "retained old anchor stays old and dirty");
    value.explicitly_saved_document_revision = maximum;
    rejected([&] { validate_recovery_copy_document(value, snapshot); });
    value.explicitly_saved_document_revision.reset(); value.document_id = "foreign";
    rejected([&] { validate_recovery_copy_document(value, snapshot); });
    auto required = Entity::create("future_required_type"); required.required = true;
    const auto read_only = Document::create({required}).snapshot();
    value.document_id = read_only.document_id();
    require(!read_only.is_editable(), "read-only provenance fixture");
    validate_recovery_copy_document(value, read_only);
}
void check_schema() {
    const auto wire = encode_recovery_copy_record(fixture());
    for (auto it = wire.begin(); it != wire.end(); ++it) {
        auto bad = wire; bad.erase(it.key());
        rejected([&] { (void)decode_recovery_copy_record(bad); });
    }
    auto bad = wire; bad["extra"] = true;
    rejected([&] { (void)decode_recovery_copy_record(bad); });
    for (const Json shape : {Json(nullptr), Json::array(), Json("record")})
        rejected([&] { (void)decode_recovery_copy_record(shape); });
    for (const auto* key : {"version", "replay_version"}) {
        for (const Json invalid : {Json(0), Json(-1), Json(true), Json(1.5), Json("1"), Json(nullptr)}) {
            bad = wire; bad[key] = invalid;
            rejected([&] { (void)decode_recovery_copy_record(bad); });
            bad[key == std::string_view("version") ? "replay_version" : "version"] = 9;
            rejected([&] { (void)decode_recovery_copy_record(bad); });
        }
    }
    for (const auto* key : {"explicitly_saved_document_revision", "explicit_save_generation", "workspace_epoch",
                            "edited_generation", "checkpoint_generation", "autosaved_checkpoint_generation", "saved_edited_generation"}) {
        for (const Json invalid : {Json(-1), Json(true), Json(0.5), Json("0")}) {
            bad = wire; bad[key] = invalid;
            rejected([&] { (void)decode_recovery_copy_record(bad); });
        }
        if (key != std::string_view("explicitly_saved_document_revision")) {
            bad = wire; bad[key] = nullptr;
            rejected([&] { (void)decode_recovery_copy_record(bad); });
        }
    }
    for (const auto* key : {"archive_id", "owner_token", "document_id"}) {
        for (const Json invalid : {Json(""), Json(std::string(129, 'x')), Json(3)}) {
            bad = wire; bad[key] = invalid;
            rejected([&] { (void)decode_recovery_copy_record(bad); });
        }
    }
    for (const Json role : {Json("named"), Json(nullptr), Json(1)}) {
        bad = wire; bad["role"] = role;
        rejected([&] { (void)decode_recovery_copy_record(bad); });
    }
    for (const auto* key : {"ownership", "extensions"}) {
        bad = wire; bad[key] = Json::array();
        rejected([&] { (void)decode_recovery_copy_record(bad); });
    }
    for (const Json path : {Json(""), Json(std::string(131073, 'x')), Json(std::string("a\0b", 3)),
                           Json(std::string(1, static_cast<char>(0xff))), Json(1)}) {
        bad = wire; bad["source_path"] = path; bad["source_sha256"] = std::string(64, 'a');
        rejected([&] { (void)decode_recovery_copy_record(bad); });
    }
    for (const Json hash : {Json(""), Json(std::string(64, 'A')), Json(std::string(64, 'z')), Json(1)}) {
        bad = wire; bad["source_path"] = "../unopened/source"; bad["source_sha256"] = hash;
        rejected([&] { (void)decode_recovery_copy_record(bad); });
    }
    for (const auto* key : {"autosaved_checkpoint_generation", "saved_edited_generation", "explicit_save_generation"}) {
        bad = wire; bad[key] = 1;
        rejected([&] { (void)decode_recovery_copy_record(bad); });
    }
    auto value = fixture(); value.owner_token.clear();
    rejected([&] { (void)encode_recovery_copy_record(value); });
    value = fixture(); value.autosaved_checkpoint_generation = 1;
    rejected([&] { (void)encode_recovery_copy_record(value); });
    value = fixture(); value.source_path = std::string("a\0b", 3);
    rejected([&] { (void)encode_recovery_copy_record(value); });
    value = fixture(); value.source_sha256 = std::string(64, 'A');
    rejected([&] { (void)encode_recovery_copy_record(value); });
}
void check_opaque_and_resources() {
    for (const auto* key : {"version", "replay_version"}) {
        Json future{{"version", 1}, {"replay_version", 1}, {"role", false}, {"payload", Json::array({1, "x"})}};
        future[key] = std::numeric_limits<std::uint64_t>::max();
        const auto decoded = decode_recovery_copy_record(future);
        require(decoded.opaque() && !decoded.supported() && *decoded.original_envelope == future &&
                !decoded.diagnostic.empty(), "future record stays wholly opaque");
        auto policy = boundary_authoring_default_resource_policy;
        policy.max_encoded_bytes = future.dump().size() - 1;
        rejected([&] { (void)decode_recovery_copy_record(future, policy); });
    }
    const auto value = fixture(); const auto wire = encode_recovery_copy_record(value);
    for (int limit = 0; limit < 4; ++limit) {
        auto policy = boundary_authoring_default_resource_policy;
        if (limit == 0) policy.max_encoded_bytes = wire.dump().size() - 1;
        if (limit == 1) policy.max_json_depth = 1;
        if (limit == 2) policy.max_json_values = 2;
        if (limit == 3) policy.max_string_bytes = 2;
        rejected([&] { (void)decode_recovery_copy_record(wire, policy); });
        rejected([&] { (void)encode_recovery_copy_record(value, policy); });
    }
    for (const auto* key : {"ownership", "extensions"}) {
        auto bad = wire; bad[key]["number"] = std::numeric_limits<double>::infinity();
        rejected([&] { (void)decode_recovery_copy_record(bad); });
        bad["version"] = 2;
        rejected([&] { (void)decode_recovery_copy_record(bad); });
        auto invalid = value;
        (key == std::string_view("ownership") ? invalid.ownership : invalid.extensions)["number"] =
            std::numeric_limits<double>::quiet_NaN();
        rejected([&] { (void)encode_recovery_copy_record(invalid); });
    }
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try { check_round_trip_and_history(); check_schema(); check_opaque_and_resources(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
