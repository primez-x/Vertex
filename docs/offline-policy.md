# Offline startup policy

`evaluate_offline_policy` is a deterministic, side-effect-free startup gate. It requires
explicit declarations that the runtime needs no account, activation, subscription or
network. Each declaration begins as `unknown`, which blocks startup. A `required`
declaration or invalid enum also blocks startup. Every policy flag defaults to true;
turning a flag off is an invalid weakening and blocks startup.

The application should construct `RuntimeCapabilities` from its actual compiled runtime
contract, evaluate before starting document workflows, and stop with the returned
diagnostics when `startup_allowed` is false. Setting a declaration to `not_required`
is an assertion by the caller, not a connectivity test or evidence collected by this
module. The gate cannot discover hidden dependencies or enforce a network sandbox.

Optional local resources (for example a local AI model or material library) have
`available`, `unavailable`, and `unknown` statuses. Missing or unchecked resources
produce warnings and permit startup; their dependent optional features should remain
disabled. Required local storage or other essential prerequisites must be checked by
the caller separately. Malformed declarations, duplicate resource identifiers, and
invalid statuses fail closed. Resource identifiers are limited to 64 lowercase ASCII
letters, digits, underscores or hyphens; use stable names, never private paths or data.

`offline_policy_json` reevaluates the same inputs and emits a version 1 JSON envelope
containing the policy, runtime requirements, optional resources, verdict and diagnostics.
Ordering is deterministic for identical inputs: dependency diagnostics follow account,
activation, subscription, network order; resource diagnostics follow input order.
Invalid resource identifiers are redacted. No JSON parser or persisted configuration
override is exposed, so diagnostic output cannot be loaded to weaken startup policy.

This module performs no network, file-system, authentication, activation, subscription,
telemetry or resource discovery operations. The application owns local-resource probes
and applying the startup verdict. Unit tests cover all dependency axes, weakened policy,
unknown and invalid states, optional resource degradation, identifier validation, and
deterministic JSON output.
