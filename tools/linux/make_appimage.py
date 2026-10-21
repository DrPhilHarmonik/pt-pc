"""The Linux AppImage (docs/linux.md) from a Linux build folder.

P.T.PC.Port-x86_64.AppImage
  AppRun                        the entry point: finds the game folder next to the .AppImage, then runs usr/bin/pt
  pt-port.desktop, pt-port.png  what the desktop shows once the AppImage is integrated
  usr/bin/pt                    the game
  usr/bin/shaders/, fonts/, voice/, lisa_date/, licenses/, libamd_fidelityfx_vk.so, libnvidia-ngx-dlss.so.*
                                what pt finds through SDL_GetBasePath(), which is usr/bin in an AppImage

Nothing is written into the image: an AppImage is a read-only squashfs mount, so pt keeps its settings, saves and pt.log
in ~/.local/share/pt-port/pt (src/main.cpp, UserDataDir, sees the runtime's APPIMAGE variable).

The image holds no Konami asset: the player's own extracted CUSA01127 folder (or a .zar of it) goes next to the
.AppImage, is named with --game, picked on the first-run screen or remembered in game_dir.txt.
"""
import argparse
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
APP_NAME = "P.T. PC Port"
# the desktop file's name, its Icon=, and the icon file's name without the extension (the AppDir spec asks for the match)
DESKTOP_ID = "pt-port"
ARCH = "x86_64"
# voice/ as the recognizer loads it on Linux (src/engine/voice/voice_recognizer.cpp): whisper.cpp and ggml, plain .so
# files with an $ORIGIN rpath that find each other in voice/, plus ggml's per-CPU variants
VOICE_MODELS = ("ggml-base.en-q5_1.bin", "ggml-small.en-q5_1.bin", "ggml-silero-v6.2.0.bin")
VOICE_LIBRARIES = ("libwhisper.so", "libggml.so", "libggml-base.so")

APPRUN = """#!/bin/sh
# The AppImage's entry point (tools/linux/make_appimage.py). pt sits in usr/bin with its shaders, fonts, the voice model,
# lisa_date and the licences beside it, and the game is read-only: its settings, saves and pt.log go to
# ~/.local/share/pt-port/pt (src/main.cpp, UserDataDir).
#
# The game files are not in the image (they are Konami's). This passes a CUSA01127 folder, or one .zar, that lies next to
# the .AppImage as --game, so a folder holding both runs from the file manager; --game on the command line still wins,
# and the first-run screen and game_dir.txt work as they do for any other copy.
set -e
appdir=${APPDIR:-$(cd "$(dirname "$0")" && pwd)}
here=$(dirname "$0")
if [ -n "${APPIMAGE:-}" ]; then
    here=$(dirname "$APPIMAGE")
fi
for folder in "$here/game/CUSA01127" "$here/CUSA01127"; do
    if [ -f "$folder/chunk1.psarc" ] && [ -f "$folder/texture.qar" ]; then
        exec "$appdir/usr/bin/pt" --game "$folder" "$@"
    fi
done
for archive in "$here"/*.zar; do
    if [ -f "$archive" ]; then
        exec "$appdir/usr/bin/pt" --game "$archive" "$@"
    fi
done
exec "$appdir/usr/bin/pt" "$@"
"""

DESKTOP = """[Desktop Entry]
Type=Application
Name={name}
GenericName=Game
Comment=A first-person horror demo, rebuilt for PC
Exec=pt
Icon={icon}
Terminal=false
Categories=Game;
Keywords=horror;pt;konami;
StartupWMClass=pt
"""


def version(build):
    header = build / "generated" / "pt_version.h"
    match = re.search(r'#define PT_VERSION "([^"]+)"', header.read_text()) if header.is_file() else None
    return match.group(1) if match else "0.0.0"


def largest_png(payload):
    """The biggest PNG image inside an .ico or .icns: both may carry their large sizes as PNG, and that is the icon
    the PS4 game ships (assets/pt.icns, assets/pt.ico). None when the file holds no PNG at all."""
    magic = b"\x89PNG\r\n\x1a\n"
    best = None
    at = payload.find(magic)
    while at >= 0:
        end = payload.find(b"IEND", at)
        if end < 0:
            break
        image = payload[at:end + 8]
        width, height = struct.unpack(">II", image[16:24])
        if best is None or width * height > best[0]:
            best = (width * height, image)
        at = payload.find(magic, end + 8)
    return best[1] if best else None


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def drawn_icon(size=256):
    """A stand-in for when no PNG can be read out of assets/: the same 256x256 picture every time, so the image does not
    depend on a binary asset this script would have to decode."""
    unit = size // 16
    pixels = [[(10, 10, 12, 255)] * size for _ in range(size)]

    def box(x, y, w, h, colour):
        for row in range(max(0, y), min(size, y + h)):
            for col in range(max(0, x), min(size, x + w)):
                pixels[row][col] = colour

    plate, white, accent = (16, 16, 20, 255), (236, 234, 228, 255), (150, 24, 24, 255)
    box(0, 0, size, unit // 2, plate)
    box(0, size - unit // 2, size, unit // 2, plate)
    box(0, 0, unit // 2, size, plate)
    box(size - unit // 2, 0, unit // 2, size, plate)
    # P: stem, top bar, bowl's bottom and right
    box(4 * unit, 3 * unit, unit, 9 * unit, white)
    box(4 * unit, 3 * unit, 4 * unit, unit, white)
    box(4 * unit, 7 * unit, 4 * unit, unit, white)
    box(7 * unit, 3 * unit, unit, 5 * unit, white)
    # T: top bar and stem
    box(9 * unit, 3 * unit, 6 * unit, unit, white)
    box(11 * unit, 3 * unit, unit, 9 * unit, white)
    box(9 * unit, 13 * unit, 6 * unit, unit // 2, accent)
    raw = b"".join(b"\x00" + bytes(value for pixel in row for value in pixel) for row in pixels)
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def icon_size(image):
    """The width and height in a PNG's IHDR, for the hicolor folder's name."""
    width, height = struct.unpack(">II", image[16:24])
    return (width, height) if width > 0 and height > 0 else (256, 256)


def icon():
    """assets/pt.png when the folder has one, else the largest PNG inside the PS4 game's icon (assets/pt.icns first,
    assets/pt.ico after it), else a drawn one."""
    for name in ("pt.png", "pt.icns", "pt.ico"):
        path = REPO / "assets" / name
        if not path.is_file():
            continue
        payload = path.read_bytes()
        image = payload if payload.startswith(b"\x89PNG\r\n\x1a\n") else largest_png(payload)
        if image:
            return image
    print("assets/ holds no PNG this script can read; using a drawn icon")
    return drawn_icon()


def upscaler_runtimes(build):
    """The upscaler runtimes the build configured, next to pt (tools/package.py knows their names and reads the build's
    upscalers.txt; it warns about a backend the build left out and exits on a file it should have but lacks)."""
    sys.path.insert(0, str(REPO / "tools"))
    import package
    return package.linux_upscaler_files(build)


def copy_voice(source, target):
    missing = [n for n in VOICE_MODELS + VOICE_LIBRARIES if not (source / n).is_file()]
    variants = sorted(source.glob("libggml-cpu-*.so"))
    if not variants:
        missing.append("libggml-cpu-*.so")
    if not (source / "licenses").is_dir():
        missing.append("licenses")
    if missing:
        sys.exit(f"{source} lacks {', '.join(missing)}; build the pt target first")
    target.mkdir(parents=True)
    for name in VOICE_MODELS + VOICE_LIBRARIES:
        shutil.copy2(source / name, target / name)
    for variant in variants:
        shutil.copy2(variant, target / variant.name)
    shutil.copytree(source / "licenses", target / "licenses")


def main():
    parser = argparse.ArgumentParser(description="Make P.T.PC.Port-x86_64.AppImage from a Linux build folder")
    parser.add_argument("--build", default=str(REPO / "build" / "linux"))
    parser.add_argument("--out", default=str(REPO / "dist"))
    parser.add_argument("--exe", help="Explicit executable: pt_release for the release game (PT_RELEASE_LOCKS); shipped as pt")
    parser.add_argument("--appimagetool", help="appimagetool (or appimagetool-x86_64.AppImage); PATH and $PT_APPIMAGETOOL "
                                               "are tried first")
    args = parser.parse_args()
    if platform.machine() not in ("x86_64", "amd64"):
        sys.exit(f"this packs the x86-64 build; {platform.machine()} is not that")
    build = Path(args.build)
    exe = Path(args.exe) if args.exe else build / "pt"
    if not exe.is_file():
        sys.exit(f"{exe} not found; build first: cmake --build build/linux --target pt")
    shaders = sorted((build / "shaders").glob("*.spv"))
    if not shaders:
        sys.exit(f"{build / 'shaders'} holds no .spv; build the pt target (pt_shaders compiles them)")
    ver = version(build)
    out = Path(args.out)
    image = out / f"P.T.PC.Port-{ARCH}.AppImage"
    if image.exists():
        sys.exit(f"Package output already exists; choose another --out folder: {image}")
    appdir = out / "AppDir"
    if appdir.exists():
        shutil.rmtree(appdir)
    bindir = appdir / "usr" / "bin"
    bindir.mkdir(parents=True)
    shutil.copy2(exe, bindir / "pt")
    (bindir / "pt").chmod(0o755)
    (bindir / "shaders").mkdir()
    for spv in shaders:
        shutil.copy2(spv, bindir / "shaders" / spv.name)
    copy_voice(build / "voice", bindir / "voice")
    for folder in ("fonts", "lisa_date"):
        if not (build / folder).is_dir():
            sys.exit(f"{build / folder} not found; build the pt target (it copies it there)")
        shutil.copytree(build / folder, bindir / folder)
    if (build / "licenses").is_dir():
        shutil.copytree(build / "licenses", bindir / "licenses",
                        ignore=shutil.ignore_patterns("enhanced-textures", "Real-ESRGAN-*.txt", "bc7enc.txt"))
    runtimes = upscaler_runtimes(build)
    for runtime in runtimes:
        shutil.copy2(runtime, bindir / runtime.name)
    shutil.copy2(REPO / "docs" / "linux.md", bindir / "Linux.md")
    shutil.copy2(REPO / "README.md", bindir / "README.md")
    # the entry point, the desktop entry and the icon the AppDir spec asks for (Icon= without the extension)
    (appdir / "AppRun").write_text(APPRUN)
    (appdir / "AppRun").chmod(0o755)
    entry = DESKTOP.format(name=APP_NAME, icon=DESKTOP_ID)
    (appdir / f"{DESKTOP_ID}.desktop").write_text(entry)
    picture = icon()
    (appdir / f"{DESKTOP_ID}.png").write_bytes(picture)
    shutil.copy2(appdir / f"{DESKTOP_ID}.png", appdir / ".DirIcon")
    # the same two files where a desktop that installs the AppImage looks for them (AppImageLauncher and friends)
    (appdir / "usr" / "share" / "applications").mkdir(parents=True)
    (appdir / "usr" / "share" / "applications" / f"{DESKTOP_ID}.desktop").write_text(entry)
    width, height = icon_size(picture)
    hicolor = appdir / "usr" / "share" / "icons" / "hicolor" / f"{width}x{height}" / "apps"
    hicolor.mkdir(parents=True)
    shutil.copy2(appdir / f"{DESKTOP_ID}.png", hicolor / f"{DESKTOP_ID}.png")
    tool = args.appimagetool or os.environ.get("PT_APPIMAGETOOL") or shutil.which("appimagetool")
    if not tool:
        sys.exit(f"appimagetool not found: put it on PATH or pass --appimagetool (the AppDir is in {appdir})")
    # a runner has no FUSE: the AppImage runtime unpacks appimagetool itself instead of mounting it
    env = dict(os.environ, ARCH=ARCH, APPIMAGE_EXTRACT_AND_RUN="1")
    subprocess.run([tool, str(appdir), str(image)], check=True, env=env)
    size = image.stat().st_size / (1024 * 1024)
    print(f"{appdir}\n{image} ({size:.1f} MB, version {ver}, {len(shaders)} shaders, "
          f"{len(runtimes)} upscaler runtime{'s' if len(runtimes) != 1 else ''})")


if __name__ == "__main__":
    main()
