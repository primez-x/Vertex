#include "sketch/assistance_contract.hpp"

#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>

namespace sketch {
namespace {
using Json = nlohmann::json;
void check(bool value, const char* message) {
    if (!value) throw std::invalid_argument(message);
}
void text(const std::string& value, std::size_t limit = 4096) {
    check(!value.empty() && value.size() <= limit &&
          value.find_first_of("\r\n\t") == std::string::npos &&
          value.find('\0') == std::string::npos, "Invalid assistance text");
}
std::string kind_name(AssistanceKind kind) {
    switch (kind) {
    case AssistanceKind::tracing: return "tracing";
    case AssistanceKind::dimension_extraction: return "dimension_extraction";
    case AssistanceKind::label_placement: return "label_placement";
    case AssistanceKind::natural_language: return "natural_language";
    }
    throw std::invalid_argument("Unknown assistance kind");
}
void keys(const Json& value, std::initializer_list<std::string_view> names) {
    check(value.is_object() && value.size() == names.size(), "Invalid assistance object fields");
    for (auto name : names) check(value.contains(std::string(name)), "Missing assistance field");
}
void local_path(const std::string& value) {
    text(value);
    check(value.front() != '/' && value.find_first_of("\\:") == std::string::npos,
          "Assistance resources require portable local paths");
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find('/', start);
        const auto part = value.substr(start, end == std::string::npos ? end : end - start);
        check(!part.empty() && part != "." && part != ".." &&
              part.back() != '.' && part.back() != ' ', "Invalid assistance path component");
        if (end == std::string::npos) break;
        start = end + 1;
    }
}
void arguments(const Json& value, unsigned depth, std::size_t& nodes) {
    check(depth <= 16 && ++nodes <= 4096, "Oversized assistance command arguments");
    check(!value.is_discarded() && !value.is_binary(), "Unsupported assistance argument");
    if (value.is_number_float()) check(std::isfinite(value.get<double>()), "Non-finite assistance argument");
    if (value.is_string()) check(value.get_ref<const std::string&>().size() <= 16384,
                                "Oversized assistance argument string");
    if (value.is_structured()) {
        for (auto i = value.begin(); i != value.end(); ++i) {
            if (value.is_object()) text(i.key(), 256);
            arguments(i.value(), depth + 1, nodes);
        }
    }
}
} // namespace

void validate_assistance_proposal(const AssistanceProposal& p) {
    text(p.id, 256); text(p.producer); (void)kind_name(p.kind);
    text(p.source.reference_id, 256);
    check(p.source.original_text.size() <= 16384, "Oversized assistance source text");
    const auto& s = p.source;
    check(std::isfinite(s.x) && std::isfinite(s.y) && std::isfinite(s.width) &&
          std::isfinite(s.height) && std::isfinite(s.confidence) && s.x >= 0 && s.y >= 0 &&
          s.width > 0 && s.height > 0 && s.x + s.width <= 1 && s.y + s.height <= 1 &&
          s.confidence >= 0 && s.confidence <= 1, "Invalid assistance source location or confidence");
    if (p.kind == AssistanceKind::dimension_extraction || p.kind == AssistanceKind::natural_language)
        check(!s.original_text.empty(), "Extracted dimensions and language require original source text");
    check(!p.resources.empty() && p.resources.size() <= 64, "Assistance requires bounded resource declarations");
    std::set<std::string> ids;
    for (const auto& resource : p.resources) {
        text(resource.id, 256); text(resource.provenance); text(resource.license);
        check(ids.insert(resource.id).second, "Duplicate assistance resource ID");
        if (resource.included) local_path(resource.relative_path);
        else check(resource.relative_path.empty(), "Omitted assistance resource has a path");
    }
    text(p.preview.command_type, 256);
    check(!p.preview.affected_entity_ids.empty() && p.preview.affected_entity_ids.size() <= 1024,
          "Assistance preview must identify bounded affected entities");
    ids.clear();
    for (const auto& id : p.preview.affected_entity_ids) {
        text(id, 256); check(ids.insert(id).second, "Duplicate assistance affected entity");
    }
    check(p.preview.arguments.is_object(), "Assistance command arguments must be an object");
    std::size_t nodes = 0;
    arguments(p.preview.arguments, 0, nodes);
}

Json encode_assistance_proposal(const AssistanceProposal& p) {
    validate_assistance_proposal(p);
    Json resources = Json::array();
    for (const auto& r : p.resources) resources.push_back({{"id",r.id},{"relative_path",r.relative_path},
        {"provenance",r.provenance},{"license",r.license},{"included",r.included}});
    const auto& s = p.source;
    return {{"schema_version",1},{"status","unverified"},{"requires_explicit_acceptance",true},
        {"id",p.id},{"kind",kind_name(p.kind)},{"producer",p.producer},{"resources",resources},
        {"source",{{"reference_id",s.reference_id},{"original_text",s.original_text},{"x",s.x},
            {"y",s.y},{"width",s.width},{"height",s.height},{"confidence",s.confidence}}},
        {"preview",{{"command_type",p.preview.command_type},
            {"affected_entity_ids",p.preview.affected_entity_ids},{"arguments",p.preview.arguments}}}};
}

AssistanceProposal decode_assistance_proposal(const Json& j) {
    try {
        keys(j, {"schema_version","status","requires_explicit_acceptance","id","kind","producer","resources","source","preview"});
        check(j.at("schema_version").is_number_integer() && j.at("schema_version") == 1,
              "Unsupported assistance schema");
        check(j.at("status") == "unverified" && j.at("requires_explicit_acceptance").is_boolean() &&
              j.at("requires_explicit_acceptance") == true, "Assistance cannot claim verification or bypass acceptance");
        AssistanceProposal p;
        p.id = j.at("id").get<std::string>(); p.producer = j.at("producer").get<std::string>();
        const auto name = j.at("kind").get<std::string>();
        bool found = false;
        for (auto kind : {AssistanceKind::tracing, AssistanceKind::dimension_extraction,
                          AssistanceKind::label_placement, AssistanceKind::natural_language})
            if (kind_name(kind) == name) { p.kind = kind; found = true; }
        check(found, "Unknown assistance kind");
        const auto& resources = j.at("resources");
        check(resources.is_array() && resources.size() <= 64, "Invalid assistance resources");
        for (const auto& r : resources) {
            keys(r, {"id","relative_path","provenance","license","included"});
            p.resources.push_back({r.at("id").get<std::string>(),r.at("relative_path").get<std::string>(),
                r.at("provenance").get<std::string>(),r.at("license").get<std::string>(),r.at("included").get<bool>()});
        }
        const auto& s = j.at("source");
        keys(s, {"reference_id","original_text","x","y","width","height","confidence"});
        p.source = {s.at("reference_id").get<std::string>(),s.at("original_text").get<std::string>(),
            s.at("x").get<double>(),s.at("y").get<double>(),s.at("width").get<double>(),
            s.at("height").get<double>(),s.at("confidence").get<double>()};
        const auto& preview = j.at("preview");
        keys(preview, {"command_type","affected_entity_ids","arguments"});
        p.preview = {preview.at("command_type").get<std::string>(),
            preview.at("affected_entity_ids").get<std::vector<std::string>>(),preview.at("arguments")};
        validate_assistance_proposal(p);
        return p;
    } catch (const Json::exception& e) {
        throw std::invalid_argument(std::string("Invalid assistance JSON: ") + e.what());
    }
}

std::vector<std::string> missing_assistance_resources(const AssistanceProposal& p,
    const std::vector<std::string>& available) {
    validate_assistance_proposal(p);
    const std::set<std::string> available_set(available.begin(), available.end());
    std::vector<std::string> missing;
    for (const auto& r : p.resources)
        if (!r.included || !available_set.contains(r.id)) missing.push_back(r.id);
    return missing;
}

AssistanceCommandRequest AssistanceSession::request_acceptance(const AssistanceProposal& p,
    bool accepted, const std::vector<std::string>& available) const {
    check(enabled_, "Assistance is disabled");
    check(accepted, "Assistance requires explicit user acceptance");
    check(missing_assistance_resources(p, available).empty(), "Assistance resources are unavailable");
    return {p, true, true, true};
}
} // namespace sketch
