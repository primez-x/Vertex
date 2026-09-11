#include "sketch/import_worker_policy.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void run() {
    sketch::ImportWorkerPolicy policy;
    policy.temporary_root = "C:/worker-temp/job-123";
    policy.module_search_roots = {"C:/Program Files/Sketch/worker"};
    sketch::ImportWorkerAttestation attestation{true, true, true, true, true, true, true, true, true};
    sketch::ImportWorkerInput input;
    input.relative_name = "layers/map.dxf";
    input.input_bytes = 100;
    input.expanded_bytes = 10'000;
    auto evaluate = [&] { return sketch::evaluate_import_worker(policy, attestation, input); };
    check(evaluate().allowed, "valid attested policy rejected");
    const auto good = evaluate().to_json().dump();
    check(good == "{\"allowed\":true,\"diagnostics\":[],\"network_requests_permitted\":false,\"schema_version\":1,\"terminate_worker\":false}", "canonical JSON changed");
    check(!sketch::evaluate_import_worker(policy, {}, input).allowed, "unattested launch allowed");
    for (auto field : {&sketch::ImportWorkerAttestation::restricted_token_verified,
        &sketch::ImportWorkerAttestation::network_denial_verified, &sketch::ImportWorkerAttestation::job_limits_verified,
        &sketch::ImportWorkerAttestation::parent_exit_kills_job_verified, &sketch::ImportWorkerAttestation::brokered_handles_verified,
        &sketch::ImportWorkerAttestation::private_temporary_root_verified, &sketch::ImportWorkerAttestation::immutable_module_roots_verified,
        &sketch::ImportWorkerAttestation::fixed_search_applied, &sketch::ImportWorkerAttestation::proj_offline_applied}) {
        attestation.*field = false;
        check(!evaluate().allowed && evaluate().terminate_worker, "missing control allowed");
        attestation.*field = true;
    }
    for (const char* path : {"../bad", "folder/../bad", "C:/bad", "//server/bad", "a:stream", "NUL.txt", "a/CON", "a/", "a//b", "a. /b", "a\\..\\b"}) {
        input.relative_name = path;
        check(!evaluate().allowed, "unsafe archive path accepted");
    }
    input.relative_name = "layers/map.dxf";
    input.expanded_bytes++;
    check(!evaluate().allowed, "fractional ratio overflow accepted");
    input.expanded_bytes = std::numeric_limits<std::uint64_t>::max();
    check(!evaluate().allowed, "decompression bomb accepted");
    input.expanded_bytes = 1;
    input.input_bytes = 0;
    check(!evaluate().allowed, "zero-byte bomb accepted");
    input.input_bytes = policy.max_input_bytes + 1;
    check(!evaluate().allowed, "oversized input accepted");
    input.input_bytes = 100;
    input.elapsed_ms = policy.timeout_ms;
    check(!evaluate().allowed, "deadline accepted");
    input.elapsed_ms = 0;
    input.child_processes = 1;
    check(!evaluate().allowed, "child escape accepted");
    input.child_processes = 0;
    input.malformed = true;
    check(!evaluate().allowed, "malformed input accepted");
    input.malformed = false;
    input.crashed = true;
    check(!evaluate().allowed, "crash accepted");
    input.crashed = false;
    input.required_proj_resources = {"grids/local.tif"};
    check(evaluate().diagnostics == std::vector<std::string>{"missing_local_proj_resource"}, "missing resource diagnostic wrong");
    input.bundled_proj_resources = input.required_proj_resources;
    check(evaluate().allowed, "bundled PROJ resource rejected");
    policy.proj_network = true;
    check(!evaluate().allowed, "PROJ networking accepted");
    policy.proj_network = false;
    policy.proj_custom_network_callbacks = true;
    check(!evaluate().allowed, "custom PROJ callbacks accepted");
    policy.proj_custom_network_callbacks = false;
    policy.network_capabilities = true;
    check(!evaluate().allowed, "network capability accepted");
    policy.network_capabilities = false;
    policy.max_active_processes = 2;
    check(!evaluate().allowed, "multi-process job accepted");
    policy.max_active_processes = 1;
    policy.module_search_roots = {"."};
    check(!evaluate().allowed, "relative module root accepted");
    policy.module_search_roots = {"C:/Program Files/Sketch/worker"};
    policy.temporary_root = "C:/temp/../escape";
    check(!evaluate().allowed, "temporary root traversal accepted");
    policy.temporary_root = "C:/worker-temp/job-123";
    policy.timeout_ms = 0;
    check(!evaluate().allowed, "unbounded timeout accepted");
    policy.timeout_ms = 30'000;
    check(evaluate().allowed, "failure contaminated subsequent input");
    input.required_proj_resources = {"missing-b", "missing-a", "missing-b"};
    const auto report = evaluate().to_json().dump();
    std::reverse(input.required_proj_resources.begin(), input.required_proj_resources.end());
    check(report == evaluate().to_json().dump(), "diagnostics not deterministic");
    check(nlohmann::json::parse(report).at("allowed") == false, "JSON round trip failed");
}
}
int main() {
    try { run(); std::cout << "import worker policy tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
