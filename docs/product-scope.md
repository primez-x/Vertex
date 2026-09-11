# Product scope contract

`ProductScopeProfile` is the machine-readable boundary for the initial
production target: Windows 11 x64, English, imperial and metric units,
residential and light-commercial markets, and separate Measurement and
Architectural workspaces over one project model. The production profile also
fails closed if an account, activation server, subscription, or network is
required for core work.

This declaration constrains the product; it does not certify that each listed
workflow or fixture has passed. The final acceptance gate still requires
representative projects, device/input checks, and integrated create/edit/save/
reopen/print/export evidence for both markets and both workspaces.

Version 1 uses strict deterministic JSON with no unknown fields. Callers must
validate the profile before using it as a build or installer claim. A profile
cannot be narrowed to one unit system, market, or workspace and still satisfy
the production scope.
