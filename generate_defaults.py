"""Generate the missing-file defaults from the packaged CFG before compilation."""

from pathlib import Path
import json

root = Path(__file__).resolve().parent
config = (root / "AntiWallHack.cfg").read_text(encoding="utf-8")
content = "#pragma once\nnamespace awh {\ninline constexpr const char* DefaultConfig = " + json.dumps(config) + ";\n}\n"
target = root / "src/Config/Defaults.h"
if not target.exists() or target.read_text(encoding="utf-8") != content:
    target.write_text(content, encoding="utf-8")
