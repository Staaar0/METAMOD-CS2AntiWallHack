"""Fetch the pinned build dependencies. Requires Python 3 and Git."""

from pathlib import Path
import subprocess
import urllib.request

root = Path(__file__).resolve().parent
deps = root / "deps"
deps.mkdir(exist_ok=True)
for name, url, branch, revision, recursive in [
    (
        "metamod-source",
        "https://github.com/alliedmodders/metamod-source.git",
        "master",
        "170058a2f0dbd58106b7c76bfe88a94672295356",
        True,
    ),
    (
        "hl2sdk-cs2",
        "https://github.com/alliedmodders/s2sdk.git",
        "cs2",
        "d4a8b46676ded25b9d956287ec40ef3c9e12c14c",
        False,
    ),
]:
    path = deps / name
    if path.exists():
        print(f"Using existing {path}; ensure it matches documented revision {revision}.")
        continue
    subprocess.run(["git", "clone", "--branch", branch, url, str(path)], check=True)
    subprocess.run(["git", "-C", str(path), "checkout", revision], check=True)
    if recursive:
        subprocess.run(["git", "-C", str(path), "submodule", "update", "--init", "--recursive"], check=True)
jsondir = deps / "json"
jsondir.mkdir(exist_ok=True)
if not (jsondir / "json.hpp").exists():
    urllib.request.urlretrieve(
        "https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp", jsondir / "json.hpp"
    )
print("Build dependencies ready.")
