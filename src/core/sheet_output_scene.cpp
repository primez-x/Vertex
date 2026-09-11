#include "sketch/sheet_output_scene.hpp"
#include "sketch/sheet_view_entity_codec.hpp"

#include <algorithm>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
void select_sheet(const SheetViewModel& model, const std::string& id) {
    require(std::any_of(model.sheets().begin(), model.sheets().end(),
        [&](const auto& sheet) { return sheet.id == id; }), "output sheet does not exist");
}
Json descriptor(const DocumentSnapshot& snapshot, const std::string& entity_id,
                const std::string& sheet_id) {
    const auto entity = snapshot.entities().find(entity_id);
    require(entity != snapshot.entities().end(), "output sheet/view entity does not exist");
    const auto model = decode_sheet_view_entity(entity->second);
    select_sheet(model, sheet_id);
    return {{"schema", "sketch.sheet-output-scene"}, {"version", kSheetOutputSceneVersion},
            {"entity_id", entity_id}, {"sheet_id", sheet_id}, {"definition", model.to_json()}};
}
FingerprintResource scene_resource(const Json& value) {
    const auto text = value.dump();
    const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
    return {"sketch.sheet-output-scene", sha256_hex(std::span<const std::byte>(bytes, text.size())),
            {{"schema", value.at("schema")}, {"version", value.at("version")},
             {"entity_id", value.at("entity_id")}, {"sheet_id", value.at("sheet_id")}}};
}
OutputFingerprintInputs bind_scene_inputs(const OutputFingerprintInputs& inputs, const Json& value) {
    require(inputs.views.state == FingerprintGroupState::unspecified &&
        inputs.views.resources.empty() && inputs.views.roles.empty() && inputs.views.reason.empty(),
        "sheet output adapter requires an unspecified empty views dependency group");
    auto result = inputs;
    result.views.state = FingerprintGroupState::resources;
    result.views.resources.push_back(scene_resource(value));
    return result;
}
}

Json make_sheet_output_scene(const DocumentSnapshot& snapshot, const std::string& entity_id,
    const std::string& sheet_id, const OutputFingerprintInputs& inputs) {
    auto result = descriptor(snapshot, entity_id, sheet_id);
    const auto fingerprint = make_output_fingerprint(snapshot, bind_scene_inputs(inputs, result));
    result["fingerprint"] = serialize_output_fingerprint(fingerprint);
    return result;
}

OutputFingerprintCurrentness check_sheet_output_scene_current(
    const Json& encoded, const DocumentSnapshot& snapshot, const OutputFingerprintInputs& inputs) {
    try {
        require(encoded.is_object() && encoded.size() == 6 &&
            encoded.at("schema") == "sketch.sheet-output-scene" &&
            encoded.at("version").is_number_integer() && encoded.at("version") == kSheetOutputSceneVersion &&
            encoded.at("entity_id").is_string() && encoded.at("sheet_id").is_string(),
            "invalid sheet output scene envelope");
        const auto entity_id = encoded.at("entity_id").get<std::string>();
        const auto sheet_id = encoded.at("sheet_id").get<std::string>();
        require(!entity_id.empty(), "empty output entity identity");
        const auto model = SheetViewModel::from_json(encoded.at("definition"));
        select_sheet(model, sheet_id);
        auto value = encoded;
        value.erase("fingerprint");
        // Normalize before hashing; equivalent input array order is accepted.
        value["definition"] = model.to_json();
        std::string error;
        const auto fingerprint = deserialize_output_fingerprint(encoded.at("fingerprint"), &error);
        if (!fingerprint) throw std::invalid_argument(error);
        const auto expected = scene_resource(value);
        const auto& views = fingerprint->manifest.at("dependencies").at("views");
        require(views.at("state") == "resources" && views.at("resources") == Json::array({
            {{"id", expected.id}, {"sha256", expected.sha256}, {"metadata", expected.metadata}}}),
            "output fingerprint does not bind this sheet scene");
        const auto current = descriptor(snapshot, entity_id, sheet_id);
        return check_output_fingerprint_current(*fingerprint, snapshot, bind_scene_inputs(inputs, current));
    } catch (const std::exception& error) {
        OutputFingerprintCurrentness result;
        result.error = error.what();
        return result;
    }
}
}
