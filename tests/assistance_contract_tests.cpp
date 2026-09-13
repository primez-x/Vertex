#include "sketch/assistance_contract.hpp"
#include "support/noninteractive_errors.hpp"

#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void invalid(const std::function<void()>& operation) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Malformed or unauthorized assistance was accepted");
}
sketch::AssistanceProposal fixture() {
    return {"proposal-1",sketch::AssistanceKind::tracing,"fixture-generator-v1",
        {{"processor","lib/fixture.bin","test fixture only","MIT",true}},
        {"reference-1","12 ft",0.1,0.2,0.3,0.4,0.75},
        {"add_boundary",{"new-boundary-1"},{{"length_expression","12 ft"}}}};
}
}

int main() {
    sketch::testing::noninteractive_errors();
    try {
        auto p = fixture();
        for (auto kind : {sketch::AssistanceKind::tracing,sketch::AssistanceKind::edge_tracing,
                          sketch::AssistanceKind::dimension_extraction,
                          sketch::AssistanceKind::label_placement,sketch::AssistanceKind::natural_language}) {
            p.kind = kind;
            const auto encoded = sketch::encode_assistance_proposal(p);
            require(encoded.at("status") == "unverified", "Proposal lost unverified status");
            require(sketch::decode_assistance_proposal(encoded) == p, "Proposal round trip lost provenance");
            require(sketch::encode_assistance_proposal(sketch::decode_assistance_proposal(encoded)).dump() ==
                    encoded.dump(), "Assistance encoding is not deterministic");
        }
        const auto original = sketch::encode_assistance_proposal(p);
        sketch::AssistanceSession session;
        invalid([&] { (void)session.request_acceptance(p,true,{"processor"}); });
        session.set_enabled(true);
        invalid([&] { (void)session.request_acceptance(p,false,{"processor"}); });
        invalid([&] { (void)session.request_acceptance(p,true,{}); });
        const auto request = session.request_acceptance(p,true,{"processor"});
        require(request.proposal == p && request.requires_normal_command_validation &&
                request.requires_permission_check && request.requires_undo_transaction,
                "Acceptance bypassed the normal command boundary");
        session.set_enabled(false);
        invalid([&] { (void)session.request_acceptance(p,true,{"processor"}); });
        require(sketch::encode_assistance_proposal(p) == original, "Acceptance mutated the proposal");
        p.resources[0].included = false; p.resources[0].relative_path.clear();
        require(sketch::missing_assistance_resources(p,{"processor"}) == std::vector<std::string>{"processor"},
                "Omitted asset incorrectly considered available");
        for (const auto* path : {"../model.bin","C:/model.bin","https://host/model.bin","/model.bin",
                                 "assets/../model.bin","assets\\model.bin","assets//model.bin"}) {
            p = fixture(); p.resources[0].relative_path = path;
            invalid([&] { sketch::validate_assistance_proposal(p); });
        }
        p = fixture(); p.source.confidence = std::numeric_limits<double>::quiet_NaN();
        invalid([&] { sketch::validate_assistance_proposal(p); });
        p = fixture(); p.preview.arguments["bad"] = std::numeric_limits<double>::infinity();
        invalid([&] { sketch::validate_assistance_proposal(p); });
        p = fixture(); p.resources.push_back(p.resources.front());
        invalid([&] { sketch::validate_assistance_proposal(p); });
        p = fixture(); p.kind = sketch::AssistanceKind::dimension_extraction; p.source.original_text.clear();
        invalid([&] { sketch::validate_assistance_proposal(p); });
        for (const auto* field : {"status","requires_explicit_acceptance","schema_version","kind"}) {
            auto j = original; j[field] = "invalid";
            invalid([&] { (void)sketch::decode_assistance_proposal(j); });
        }
        auto j = original; j["accepted"] = true;
        invalid([&] { (void)sketch::decode_assistance_proposal(j); });
        j = original; j["source"]["confidence"] = nullptr;
        invalid([&] { (void)sketch::decode_assistance_proposal(j); });
        j = original; j["resources"][0]["license"] = "";
        invalid([&] { (void)sketch::decode_assistance_proposal(j); });
        std::cout << "assistance contract tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
