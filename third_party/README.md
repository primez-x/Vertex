# Dependency provenance

`dependencies.json` pins the directly bootstrapped source artifacts. Prepare
the cache explicitly with `python scripts/bootstrap.py`, or use `--offline`
with the supplied archives. Configuration and compilation do not fetch them.
The SQLite SHA3 digest was checked against the official download page; the
manifest additionally records our downloaded SHA256 for repeatable builds.

Vertex source is public under GPL-3.0-or-later. Third-party copyrights and license
terms continue to apply. Dynamic LGPL components require their corresponding
source, license notices, replaceability/relinking rights and any required
installation information in the eventual distribution. The complete dependency
closure and production source kit are not yet qualified. A successful local
compile is not a completed redistribution audit.

Vertex makes use of facilities provided by Open CASCADE Technology (OCCT).
The retained OCCT copyright text contains LGPL 2.1 and the Open CASCADE
Exception 1.0, identified by SPDX as
`LGPL-2.1-only WITH OCCT-exception-1.0`
([exception definition](https://spdx.org/licenses/OCCT-exception-1.0.html)).
The original installed vcpkg SPDX declaration remains unchanged; its shorter
license field does not replace the complete notice. This attribution does not
establish final relinking, static-dependency or source-delivery qualification.

SQLite's source is dedicated to the public domain:
<https://www.sqlite.org/copyright.html>.
nlohmann-json uses the MIT license:
<https://github.com/nlohmann/json/blob/v3.12.0/LICENSE.MIT>.

Optional components cannot enter a production package without a recorded
version, source hash, license, transitive audit and redistribution artifacts.
