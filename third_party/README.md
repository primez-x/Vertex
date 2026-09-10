# Dependency provenance

`dependencies.json` pins the directly bootstrapped source artifacts. Prepare
the cache explicitly with `python scripts/bootstrap.py`, or use `--offline`
with the supplied archives. Configuration and compilation do not fetch them.
The SQLite SHA3 digest was checked against the official download page; the
manifest additionally records our downloaded SHA256 for repeatable builds.

The original application remains private. Third-party copyrights and license
terms continue to apply. Dynamic LGPL components require their corresponding
source, license notices, replaceability/relinking rights and any required
installation information in the eventual distribution. The complete dependency
closure and production source kit are not yet qualified. A successful local
compile is not a completed redistribution audit.

SQLite's source is dedicated to the public domain:
<https://www.sqlite.org/copyright.html>.
nlohmann-json uses the MIT license:
<https://github.com/nlohmann/json/blob/v3.12.0/LICENSE.MIT>.

Optional components cannot enter a production package without a recorded
version, source hash, license, transitive audit and redistribution artifacts.
