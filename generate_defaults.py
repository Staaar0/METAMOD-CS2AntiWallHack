"""Generate the missing-file defaults from the packaged CFG before compilation."""

from pathlib import Path

root = Path(__file__).resolve().parent
config = (root / "AntiWallHack.cfg").read_text(encoding="utf-8")
content = (
    '#pragma once\n\nnamespace awh\n{\n    inline constexpr const char* DefaultConfig = R"awhcfg('
    + config
    + ')awhcfg";\n}\n'
)

target = root / "src/Config/Defaults.h"
if not target.exists() or target.read_text(encoding="utf-8") != content:
    target.write_text(content, encoding="utf-8")
