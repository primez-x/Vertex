#include "sketch/boundary_authoring_recovery_resource.hpp"
#include "sketch/boundary_authoring_recovery.hpp"
#include "support/noninteractive_errors.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace sketch;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejected(F&& f) {
    try { f(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("expected resource rejection");
}
BoundaryAuthoringSession lines(std::size_t count) {
    BoundaryAuthoringSession s(BoundaryAuthoringMode::draw_first);
    (void)s.anchor({0,0});
    for (std::size_t i=1; i<=count; ++i) (void)s.add_line_to({static_cast<double>(i),0});
    return s;
}
void compact_linear_accounting() {
    const auto small=lines(1024); const auto large=lines(2048);
    const auto a=small.resource_usage(); const auto b=large.resource_usage();
    require(b.retained_history_bytes < a.retained_history_bytes*3,
            "compact retained charge must grow linearly");
    require(b.cumulative_replay_copy_bytes < a.cumulative_replay_copy_bytes*3,
            "compact replay charge must grow linearly");
    require(b.retained_history_bytes <= large.resource_policy().max_retained_history_bytes,
            "2048-edge calibration fixture must fit default retained ceiling");
    const auto cp=large.recovery_checkpoint();
    const auto report=estimate_boundary_authoring_recovery_resources(cp);
    require(report.retained_history_bytes==b.retained_history_bytes,
            "offline canonical estimate and live compact accounting must agree");
    const auto encoded=encode_boundary_authoring_recovery(cp,large.resource_policy());
    require(decode_boundary_authoring_recovery(encoded,large.resource_policy()).supported(),
            "2048-edge default calibration must roundtrip");
    std::cout << "1024 retained=" << a.retained_history_bytes << " 2048 retained="
              << b.retained_history_bytes << " replay=" << b.cumulative_replay_copy_bytes << '\n';
}
void caps_and_navigation() {
    auto source=lines(20);
    const auto usage=source.resource_usage();
    auto policy=source.resource_policy();
    policy.max_retained_history_bytes=usage.retained_history_bytes;
    policy.max_operation_bytes=usage.operation_bytes;
    auto capped=BoundaryAuthoringSession::from_recovery_checkpoint(source.recovery_checkpoint(),policy);
    const auto before=capped.recovery_checkpoint();
    rejected([&]{(void)capped.add_line_to({21,0});});
    require(capped.recovery_checkpoint()==before,"rejected byte admission must roll back");
    while(capped.undo()) require(capped.resource_usage()==usage,"undo zipper must already be reserved");
    while(capped.redo()) require(capped.resource_usage()==usage,"redo must retain identical charge");
    require(capped.undo(),"branch prerequisite");
    (void)capped.add_line_to({20,1});
    require(!capped.can_redo(),"branch must replace redo charges");
    require(estimate_boundary_authoring_recovery_resources(capped.recovery_checkpoint(), policy)==
                capped.resource_usage(),
            "transient discarded redo charges must not enter canonical usage");
    while (capped.undo()) {}
    (void)capped.anchor({0,1});
    require(capped.resource_usage().retained_history_bytes < usage.retained_history_bytes,
            "replacing a large redo branch must release its permanent charge");
    require(estimate_boundary_authoring_recovery_resources(capped.recovery_checkpoint(), policy)==
                capped.resource_usage(),
            "a short replacement must have the same usage after canonical replay");
    auto too_small=policy; too_small.max_retained_history_bytes=usage.retained_history_bytes-1;
    rejected([&]{(void)BoundaryAuthoringSession::from_recovery_checkpoint(before,too_small);});
    rejected([&]{(void)encode_boundary_authoring_recovery(before,too_small);});
    const auto encoded=encode_boundary_authoring_recovery(before,policy);
    rejected([&]{(void)decode_boundary_authoring_recovery(encoded,too_small);});
    for (int limit=0;limit<3;++limit) {
        auto p=policy;
        if(limit==0)p.max_operation_bytes=usage.operation_bytes-1;
        if(limit==1)p.max_materialization_bytes=usage.materialization_bytes-1;
        if(limit==2)p.max_cumulative_replay_copy_bytes=usage.cumulative_replay_copy_bytes-1;
        rejected([&]{(void)BoundaryAuthoringSession::from_recovery_checkpoint(before,p);});
    }
}
void arithmetic_and_invalid_input() {
    std::size_t out=7;
    const auto max=std::numeric_limits<std::size_t>::max();
    require(!boundary_authoring_recovery_checked_add(max,1,out)&&out==7,"checked add must preserve result");
    require(!boundary_authoring_recovery_checked_multiply(max,2,out)&&out==7,"checked multiply must preserve result");
    bool saturated=false;
    require(boundary_authoring_recovery_saturating_add(max,1,saturated)==max && saturated,
            "saturating add must report overflow");
    require(boundary_authoring_recovery_saturating_add(1,2,saturated)==3 && saturated,
            "successful add must preserve a prior overflow flag");
    require(boundary_authoring_recovery_saturating_multiply(max,0,saturated)==0 && saturated,
            "successful multiply must preserve a prior overflow flag");
    saturated=false;
    require(boundary_authoring_recovery_saturating_multiply(max,2,saturated)==max && saturated,
            "saturating multiply must report overflow");
    saturated=false;
    require(boundary_authoring_recovery_saturating_add(2,3,saturated)==5 && !saturated,
            "successful arithmetic must leave a clear overflow flag clear");
    auto cp=lines(1).recovery_checkpoint(); cp.actions.back().receipt.reset();
    rejected([&]{(void)estimate_boundary_authoring_recovery_resources(cp);});
    cp=lines(1).recovery_checkpoint(); cp.actions.back().generated_ids.front()="tampered";
    rejected([&]{(void)estimate_boundary_authoring_recovery_resources(cp);});
}
void bounded_estimation_and_deep_extensions() {
    const auto source=lines(3);
    auto cp=source.recovery_checkpoint();
    auto policy=source.resource_policy();
    policy.max_actions=1;
    const auto allocations=source.structural_stats().sequence_chunk_allocations;
    rejected([&]{(void)estimate_boundary_authoring_recovery_resources(cp,policy);});
    require(source.structural_stats().sequence_chunk_allocations==allocations,
            "estimator must reject raw limits before allocating replay sequence chunks");
    policy=source.resource_policy();
    policy.max_retained_history_bytes=source.resource_usage().retained_history_bytes-1;
    rejected([&]{(void)estimate_boundary_authoring_recovery_resources(cp,policy);});

    auto* cursor=&cp.extensions;
    for (int depth=0;depth<2000;++depth) {
        (*cursor)["child"]=nlohmann::json::object();
        cursor=&(*cursor)["child"];
    }
    rejected([&]{(void)estimate_boundary_authoring_recovery_resources(cp);});
    rejected([&]{(void)BoundaryAuthoringSession::from_recovery_checkpoint(cp);});
    rejected([&]{(void)encode_boundary_authoring_recovery(cp);});
    auto destination=lines(1);
    const auto before=destination.recovery_checkpoint();
    rejected([&]{destination.restore_recovery_checkpoint(cp);});
    require(destination.recovery_checkpoint()==before,
            "excessive typed extension depth must preserve the destination");

    policy=source.resource_policy(); policy.max_json_depth=65;
    rejected([&]{BoundaryAuthoringSession invalid(BoundaryAuthoringMode::draw_first,{},policy);});
    rejected([&]{(void)estimate_boundary_authoring_recovery_resources(before,policy);});
    const nlohmann::json future{{"version",999},{"future",{{"finite",1.25}}}};
    const auto decoded=decode_boundary_authoring_recovery(future);
    require(decoded.opaque() && *decoded.original_envelope==future,
            "bounded unsupported envelopes must retain their exact JSON value");
    rejected([&]{(void)decode_boundary_authoring_recovery(future,policy);});
    auto nonfinite=future;
    nonfinite["future"]["finite"]=std::numeric_limits<double>::infinity();
    rejected([&]{(void)decode_boundary_authoring_recovery(nonfinite);});
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try { compact_linear_accounting(); caps_and_navigation(); arithmetic_and_invalid_input();
        bounded_estimation_and_deep_extensions();
        std::cout << "Compact recovery resource tests passed\n"; return 0;
    } catch(const std::exception& e) {std::cerr << e.what() << '\n'; return 1;}
}
