#!/usr/bin/env python3
"""Build VIS-bootable ISO for the A.26 Path B waveOut spike (WAVTEST.EXE)."""
import pathlib
import pycdlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC  = ROOT / "cd_root_wavtest"
OUT  = ROOT / "build" / "wavtest.iso"

iso = pycdlib.PyCdlib()
iso.new(interchange_level=1, vol_ident="WAVTEST", sys_ident="MODWIN")

for item in sorted(SRC.iterdir()):
    if not item.is_file():
        continue
    name = item.name.upper()
    iso_name = f"/{name};1" if "." in name else f"/{name}.;1"
    print(f"add: {item} -> {iso_name}")
    iso.add_file(str(item), iso_path=iso_name)

OUT.parent.mkdir(parents=True, exist_ok=True)
iso.write(str(OUT))
iso.close()
print(f"\nWROTE {OUT} ({OUT.stat().st_size} bytes)")
