#include "sketch/boundary_active_recovery.hpp"
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
    throw std::runtime_error("expected invalid active recovery rejection");
}
BoundaryActiveRecovery record(const BoundaryAuthoringSession& session) {
    return {{"document-id", 42, std::string(64, 'a'), {"property", "building", "floor", "layer"}},
            session.recovery_checkpoint(), {{"future", {{"items", Json::array({1, "two", nullptr})}}}}};
}
void round_trip(const BoundaryAuthoringSession& session) {
    const auto original = record(session);
    const auto wire = encode_boundary_active_recovery(original);
    const auto result = decode_boundary_active_recovery(wire);
    require(result.supported() && !result.opaque() && *result.active == original, "exact active round trip");
    require(encode_boundary_active_recovery(*result.active) == wire, "stable wire round trip");
    require(BoundaryAuthoringSession::from_recovery_checkpoint(result.active->checkpoint).view() ==
                session.view(), "canonical replay retains active view");
}
void check_sessions() {
    for (auto mode : {BoundaryAuthoringMode::draw_first, BoundaryAuthoringMode::define_first}) {
        BoundaryAuthoringOptions options;
        options.automatic_dimension_placement = true;
        BoundaryAuthoringSession session(mode, options);
        if (mode == BoundaryAuthoringMode::define_first) session.set_classification("living_area");
        (void)session.anchor({0, 0});
        (void)session.add_line(parse_quantity("2 m", Unit::metre), parse_angle("0 deg"));
        (void)session.add_line(parse_quantity("3 m", Unit::metre), parse_angle("90 deg"));
        round_trip(session);
        require(session.undo(), "undo fixture");
        round_trip(session);
        require(session.redo(), "redo fixture");
        round_trip(session);
    }
}
void check_schema() {
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
    const auto original = record(session);
    const auto wire = encode_boundary_active_recovery(original);
    for (const auto* key : {"version", "replay_version", "source", "checkpoint", "extensions"}) {
        auto bad = wire; bad.erase(key);
        rejected([&] { (void)decode_boundary_active_recovery(bad); });
    }
    for (const auto* key : {"version", "replay_version"}) {
        for (const Json invalid : {Json(-1), Json(0), Json(1.5), Json(true), Json("1"), Json(nullptr)}) {
            auto bad = wire; bad[key] = invalid;
            rejected([&] { (void)decode_boundary_active_recovery(bad); });
            bad[key == std::string_view("version") ? "replay_version" : "version"] = 9;
            rejected([&] { (void)decode_boundary_active_recovery(bad); });
        }
    }
    for (const auto* key : {"document_id", "revision", "authoring_digest", "context"}) {
        auto bad = wire; bad["source"].erase(key);
        rejected([&] { (void)decode_boundary_active_recovery(bad); });
    }
    for (const Json revision : {Json(-1), Json(0.5), Json(true), Json("42")}) {
        auto bad = wire; bad["source"]["revision"] = revision;
        rejected([&] { (void)decode_boundary_active_recovery(bad); });
    }
    for (const auto* key : {"document_id", "authoring_digest"}) {
        for (const Json value : {Json(""), Json(std::string(129, 'a')), Json(1)}) {
            auto bad = wire; bad["source"][key] = value;
            rejected([&] { (void)decode_boundary_active_recovery(bad); });
        }
    }
    auto bad = wire; bad["source"]["authoring_digest"] = std::string(64, 'A');
    rejected([&] { (void)decode_boundary_active_recovery(bad); });
    for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id"}) {
        bad = wire; bad["source"]["context"].erase(key);
        rejected([&] { (void)decode_boundary_active_recovery(bad); });
        bad = wire; bad["source"]["context"][key] = std::string(129, 'x');
        rejected([&] { (void)decode_boundary_active_recovery(bad); });
        bad["source"]["context"][key] = "";
        require(decode_boundary_active_recovery(bad).supported(), "incomplete context remains recoverable");
    }
    for (const auto* path : {"", "/source", "/source/context"}) {
        bad = wire; bad[Json::json_pointer(path)]["unexpected"] = true;
        rejected([&] { (void)decode_boundary_active_recovery(bad); });
    }
    bad = wire; bad["extensions"] = Json::array();
    rejected([&] { (void)decode_boundary_active_recovery(bad); });
    auto invalid = original; invalid.source.document_id.clear();
    rejected([&] { (void)encode_boundary_active_recovery(invalid); });
    invalid = original; invalid.source.authoring_digest = std::string(64, 'z');
    rejected([&] { (void)encode_boundary_active_recovery(invalid); });
    auto boundary = original;
    boundary.source.document_id = std::string(128, 'd');
    boundary.source.revision = std::numeric_limits<Revision>::max();
    boundary.source.context = {};
    boundary.checkpoint.extensions = {{"checkpoint_extra", Json::array({false, 7})}};
    require(*decode_boundary_active_recovery(encode_boundary_active_recovery(boundary)).active == boundary,
            "maximum revision, bounded ID, empty context and checkpoint extensions survive");
    boundary.source.revision = 0;
    require(decode_boundary_active_recovery(encode_boundary_active_recovery(boundary)).supported(),
            "zero source revision is valid");
}
void check_opaque_and_budgets() {
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
    const auto wire = encode_boundary_active_recovery(record(session));
    for (const auto* path : {"/version", "/replay_version", "/checkpoint/version", "/checkpoint/replay_version"}) {
        auto future = wire;
        future[Json::json_pointer(path)] = 99;
        future["source"] = "future source representation";
        const auto result = decode_boundary_active_recovery(future);
        require(result.opaque() && !result.supported() && *result.original_envelope == future,
                "future schema preserves entire active envelope without source interpretation");
        auto limits = boundary_authoring_default_resource_policy;
        limits.max_encoded_bytes = future.dump().size() - 1;
        rejected([&] { (void)decode_boundary_active_recovery(future, limits); });
        limits = boundary_authoring_default_resource_policy;
        limits.max_json_depth = 2;
        rejected([&] { (void)decode_boundary_active_recovery(future, limits); });
    }
    const Json future{{"version", 9}, {"replay_version", 8}, {"new_payload", "opaque"}};
    require(*decode_boundary_active_recovery(future).original_envelope == future,
            "unknown outer versions do not require known payload fields");
    auto limits = boundary_authoring_default_resource_policy;
    limits.max_encoded_bytes = wire.dump().size() - 1;
    rejected([&] { (void)decode_boundary_active_recovery(wire, limits); });
    rejected([&] { (void)encode_boundary_active_recovery(record(session), limits); });
    limits = boundary_authoring_default_resource_policy; limits.max_json_depth = 2;
    rejected([&] { (void)decode_boundary_active_recovery(wire, limits); });
    limits = boundary_authoring_default_resource_policy; limits.max_json_values = 3;
    rejected([&] { (void)decode_boundary_active_recovery(wire, limits); });
    limits = boundary_authoring_default_resource_policy; limits.max_string_bytes = 3;
    rejected([&] { (void)decode_boundary_active_recovery(wire, limits); });
    auto bad = wire; bad["extensions"]["number"] = std::numeric_limits<double>::infinity();
    rejected([&] { (void)decode_boundary_active_recovery(bad); });
    auto value = record(session); value.extensions["number"] = std::numeric_limits<double>::quiet_NaN();
    rejected([&] { (void)encode_boundary_active_recovery(value); });
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try { check_sessions(); check_schema(); check_opaque_and_budgets(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
