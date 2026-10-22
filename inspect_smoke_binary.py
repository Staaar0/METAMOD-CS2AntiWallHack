"""Read-only ELF/smoke inspection for the operator-supplied Linux server module."""

from pathlib import Path
import sys, zlib, hashlib, re
import json, struct

sys.path.insert(0, str(Path(__file__).resolve().parent / "tools/reverse"))
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

path = Path(sys.argv[1])
data = path.read_bytes()
print("size", len(data), "crc32", zlib.crc32(data), "sha256", hashlib.sha256(data).hexdigest(), flush=True)
with path.open("rb") as file:
    elf = ELFFile(file)
    print(
        [
            (s.name, s["sh_size"], hex(s["sh_addr"]))
            for s in elf.iter_sections()
            if s.name in (".symtab", ".dynsym", ".text", ".rodata", ".data.rel.ro", ".rela.dyn")
        ]
    )
    for section in elf.iter_sections():
        if section["sh_type"] in ("SHT_SYMTAB", "SHT_DYNSYM"):
            names = [
                (s.name, hex(s["st_value"]), s["st_size"])
                for s in section.iter_symbols()
                if "Smoke" in s.name or "smoke" in s.name
            ]
            print("symbols", names[:40])
    for match in re.finditer(rb"[\x20-\x7e]{8,}\x00", data):
        text = match.group()[:-1]
        if b"SmokeVolume" in text or (b"CSmokeGrenadeProjectile" in text and (b"::" in text or b"ZTV" in text)):
            print(hex(match.start()), text.decode()[:160])
    code = elf.get_section_by_name(".text")
    start = int(code["sh_offset"])
    end = start + int(code["sh_size"])
    delta = int(code["sh_addr"]) - start
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    matches = list(re.finditer(rb"[\x48-\x4f]\x8d[\x80-\xbf]\x98\x0e\x00\x00", data[start:end]))
    print("volume lea candidates", len(matches), flush=True)
    for match in matches[:2]:
        offset = start + match.start()
        print("candidate", hex(offset + delta))
        for ins in decoder.disasm(data[offset : offset + 150], offset + delta):
            print(hex(ins.address), ins.mnemonic, ins.op_str)
    for base, size in ((0x1452AC0, 0x170), (0x1452E30, 0xC00), (0x1454280, 0x180), (0x1468530, 0x250)):
        print("smoke code region", hex(base))
        for ins in decoder.disasm(data[base - delta : base - delta + size], base):
            if any(
                value in ins.op_str
                for value in (
                    "0x70]",
                    "0xec]",
                    "0xd4]",
                    "0xd8]",
                    "0xdc]",
                    "0x3008",
                    "0x80000",
                    "0x13",
                    "0xf08]",
                    "0xea4]",
                )
            ) or ins.mnemonic in ("shl", "sal"):
                print(hex(ins.address), ins.mnemonic, ins.op_str)
    gamedata = json.loads((Path(__file__).resolve().parent / "gamedata.jsonc").read_text())["linux"]
    for name in ("TraceShape", "GameEventManager"):
        tokens = gamedata[name].split()
        pattern = b"".join(b"." if token in ("?", "??") else re.escape(bytes.fromhex(token)) for token in tokens)
        found = [start + m.start() for m in re.finditer(pattern, data[start:end], re.DOTALL)]
        print(name, "matches", [hex(offset + delta) for offset in found])
        if name == "GameEventManager":
            for offset in found:
                target = offset + delta + 7 + struct.unpack_from("<i", data, offset + 3)[0]
                print("event LEA target", hex(target))
                for ins in decoder.disasm(data[offset : offset + 100], offset + delta):
                    print(hex(ins.address), ins.mnemonic, ins.op_str)
