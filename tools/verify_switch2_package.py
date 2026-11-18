#!/usr/bin/env python3
"""Verify libounce Switch 2 startup (OUN0), ASET assets, and link map."""
import argparse
import struct
from pathlib import Path


def verify_switch2_startup(raw: bytes) -> None:
    page = raw[:0x1000]
    if len(page) < 0x80 or page[0x10:0x14] != b"NRO0":
        raise ValueError("Not a valid NRO0 binary")
    mod = struct.unpack_from("<I", page, 4)[0]
    if mod + 0x1C > len(page) or page[mod : mod + 4] != b"MOD0":
        raise ValueError("Missing MOD0 startup header")
    at = mod + 0x1C
    for _ in range(8):
        if at + 12 > len(page):
            break
        magic = page[at : at + 4]
        if magic == b"OUN0":
            platform, version = struct.unpack_from("<II", page, at + 4)
            if platform != 2 or version != 1:
                raise ValueError(
                    f"Unexpected OUN0 startup metadata: platform={platform} version={version}"
                )
            return
        if magic[:3] != b"LNY":
            break
        at += 12
    raise ValueError("Missing Switch 2 OUN0 startup: ounce-hbmenu would warn")


def verify_aset(raw: bytes) -> None:
    if len(raw) < 0x80 or raw[0x10:0x14] != b"NRO0":
        raise ValueError("Not a valid NRO0 binary")
    asset_start = struct.unpack_from("<I", raw, 0x18)[0]
    if asset_start >= len(raw) or raw[asset_start : asset_start + 4] != b"ASET":
        raise ValueError(f"Missing ASET container at offset 0x{asset_start:x}")

    icon_off, icon_size = struct.unpack_from("<QQ", raw, asset_start + 0x08)
    if icon_size < 64 or asset_start + icon_off + icon_size > len(raw):
        raise ValueError("Invalid NRO icon range")
    icon = raw[asset_start + icon_off : asset_start + icon_off + icon_size]
    if not icon.startswith(b"\xff\xd8\xff"):
        raise ValueError("Embedded NRO icon is not JPEG")

    nacp_off, nacp_size = struct.unpack_from("<QQ", raw, asset_start + 0x18)
    if nacp_size != 0x4000 or asset_start + nacp_off + nacp_size > len(raw):
        raise ValueError(f"Unexpected NACP size: {nacp_size}")


def verify_link_map(text: str) -> None:
    text = text.replace("\\", "/")
    forbidden = [
        "/libnx/",
        "/portlibs/switch/lib/libEGL.a",
        "/portlibs/switch/lib/libglapi.a",
        "/portlibs/switch/lib/libdrm_nouveau.a",
        "/portlibs/switch/lib/libGLESv2.a",
        "/portlibs/switch/lib/libglfw3.a",
        "/portlibs/switch/lib/libav",
        "/portlibs/switch/lib/libsw",
        "libdeko3d",
        "hwcontext_nvtegra",
        "nvtegra.o",
    ]
    for path in forbidden:
        if path in text:
            sample = next((line.strip() for line in text.splitlines() if path in line), "")
            raise ValueError(f"Forbidden Switch 1 dependency in link map: {path} ({sample})")
    for path in ["/libounce/lib/", "/portlibs/switch2/lib/"]:
        if path not in text:
            raise ValueError("Missing Switch 2 dependency in link map: " + path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("nro", type=Path)
    parser.add_argument("--link-map", type=Path, required=True)
    args = parser.parse_args()
    raw = args.nro.read_bytes()
    verify_switch2_startup(raw)
    verify_aset(raw)
    verify_link_map(args.link_map.read_text(errors="replace"))
    print(
        f"PASS: Switch 2 OUN0 platform=2 version=1 ({len(raw):,} bytes); "
        "no Switch 1 startup/graphics/nvtegra libraries"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
