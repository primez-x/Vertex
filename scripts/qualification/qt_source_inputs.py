"""Read the selected public Qt release source pins; do not relabel old inputs."""
import json
from pathlib import Path
import re


def selected_archives(root, version):
    selection = json.loads((Path(root) / "third_party/qt-sdk.json").read_text(encoding="utf-8"))
    if selection.get("schema_version") != 1 or selection.get("version") != version or not re.fullmatch(r"6\.\d+\.\d+", version):
        raise ValueError("Qt source version is not the selected pinned release")
    pins = selection.get("source_archives")
    if not isinstance(pins, dict) or set(pins) != {"qtbase", "qtsvg", "qtwebengine"}:
        raise ValueError("Invalid selected Qt source module set")
    series = version.rsplit(".", 1)[0]
    for module, pin in pins.items():
        name = f"{module}-everywhere-src-{version}.tar.xz"
        url = f"https://download.qt.io/official_releases/qt/{series}/{version}/submodules/{name}"
        if (not isinstance(pin, dict) or pin.get("name") != name or pin.get("url") != url
                or type(pin.get("bytes")) is not int or not 0 < pin["bytes"] <= 1024 ** 3
                or not isinstance(pin.get("sha256"), str) or not re.fullmatch(r"[0-9a-f]{64}", pin["sha256"])):
            raise ValueError("Invalid selected Qt source archive pin")
    return pins
