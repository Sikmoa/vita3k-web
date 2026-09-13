#!/usr/bin/env python3
"""Regenerate/check assembled CPU test bytes (VitaSDK binutils, no host assembler).

  python3 vita3k/cpu/tests/generate_interpreter_cases.py --check

Set VITASDK to the SDK root if it is not /opt/vitasdk/vitasdk.
Native instruction tests use the checked-in .inc and do not require the SDK.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
prefix = str(Path(os.environ.get("VITASDK", "/opt/vitasdk/vitasdk")) / "bin/arm-vita-eabi-")
with tempfile.TemporaryDirectory(prefix="interpreter-cases-") as directory:
    obj, elf, binary = (str(Path(directory) / name) for name in ("cases.o", "cases.elf", "cases.bin"))
    subprocess.run([prefix + "as", "-mcpu=cortex-a9", "-mfpu=neon", "-o", obj,
                    str(here / "interpreter_instruction_cases.S")], check=True)
    subprocess.run([prefix + "ld", "-Ttext=0x81000000", "-e", "test_mov", "-o", elf, obj], check=True)
    subprocess.run([prefix + "objcopy", "-O", "binary", "-j", ".text", elf, binary], check=True)
    data = Path(binary).read_bytes()
    text = "// Generated from interpreter_instruction_cases.S with VitaSDK GNU binutils.\n"
    text += "constexpr uint8_t instruction_bytes[] = {\n"
    for i in range(0, len(data), 16):
        text += "    " + ", ".join(f"0x{x:02x}" for x in data[i:i + 16]) + ",\n"
    text += "};\n"
    for line in subprocess.check_output([prefix + "nm", "-g", elf], text=True).splitlines():
        address, kind, name = line.split()
        if name.startswith("test_"):
            text += f"constexpr uint32_t {name} = 0x{address}u;\n"
    target = here / "interpreter_instruction_cases.inc"
    if "--check" in sys.argv:
        if target.read_text() != text:
            sys.exit("Assembled instruction bytes differ: regenerate interpreter_instruction_cases.inc")
        print(f"Assembled instruction bytes verified ({len(data)} bytes)")
    else:
        target.write_text(text)
