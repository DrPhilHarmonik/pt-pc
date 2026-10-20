# Linux

The game (`pt`) and the installer (`pt_setup_linux`) build for Linux x86-64. I develop on Windows and check the Linux
build by cross compiling; the notes below say what runs where.

## Platform layer

Everything the engine needs from the operating system beyond SDL3 goes through `src/engine/platform`:

| Piece | Windows | Linux |
|---|---|---|
| `os.h`: files with Unicode paths, 64-bit seek, environment, process id | `_wfopen`, `_fseeki64`, `_dupenv_s` | `fopen`, `fseeko`, `getenv` |
| `os.h`: `RunProcess` (texture upscaler, installer helper) | `CreateProcessW`, no console, below-normal priority | `posix_spawn`, nice 10, SIGKILL on cancel or timeout |
| `os.h`: `FileLock` (one texture generation at a time) | file opened without sharing | `flock` |
| `http.h`: HTTPS GET (update check) | WinHTTP | the system libcurl, loaded with `dlopen` (the game starts without it) |
| Unicode fonts (Turkish, Chinese, Arabic, Russian, Ukrainian, Czech UI) | GDI + Uniscribe | HarfBuzz 14.6.0 (fetched, built into the binary) + stb_truetype, a small bidi pass for Arabic lines |
| voice recognizer (whisper.cpp) | `whisper.dll`, `ggml*.dll` in `voice/` | `libwhisper.so`, `libggml*.so` in `voice/` (RPATH `$ORIGIN`), worker thread at nice 10 |
| enhanced textures | `realesrgan-ncnn-vulkan.exe` + `vcomp140.dll` | `realesrgan-ncnn-vulkan` (the project's ubuntu release, checked by SHA-256) |
| folder picker when no game is found | `IFileOpenDialog` | none: a message box asks for `--game` |
| crash dump | `MiniDumpWriteDump` | none |

On Linux, `PT_UPSCALERS` (`cmake/UpscalersLinux.cmake`) builds NVIDIA DLSS from the SDK's Linux files
(`libnvidia-ngx-dlss.so`, `libnvidia-ngx-dlssd.so`, staged next to pt as the Windows DLLs are) and AMD FSR 3.1 from
AMD's v1.1.4 sources, whose Vulkan shader permutations AMD's `FidelityFX_SC.exe` compiles under wine, together with
`glslangValidator` from `glslang-tools` (`apt-get install wine glslang-tools`); without either CMake warns and leaves
FSR out. Frame generation and Intel XeSS stay Windows-only, as do the DirectX 12 backend and the D3D12 game. Vulkan
itself is loaded by volk at run time; SDL3 loads X11, Wayland, ALSA, PulseAudio and PipeWire at run time.

Settings, saves and `pt.log` live in `data/` next to pt (a build folder, an unzipped portable copy), and in
`~/.local/share/pt-port/pt/` in the AppImage, which is a read-only mount.

## Building

Native, on a Linux machine (X11 and Wayland): `cmake -G Ninja -B build/linux -DCMAKE_BUILD_TYPE=RelWithDebInfo` then
`cmake --build build/linux --parallel 6 --target pt`. Needs a C++20 compiler with `<format>` (GCC 13+ or clang 17+),
the Vulkan headers and `glslc`.

Cross build on Windows:

1. `python tools/linux/make_sysroot.py` (in the folder where the sysroot should go; it reads `Packages.xz` of Debian trixie
   amd64 from deb.debian.org and verifies every package's SHA-256): headers and libraries of 87 packages, about 335 MB.
2. `PT_LINUX_SYSROOT=<sysroot> PT_DEPS=<a Windows build's _deps> sh tools/linux/cross_build.sh [target]`: LLVM clang and
   lld, CMake, Ninja, at most 6 jobs (`PT_JOBS`). The toolchain file is `cmake/toolchains/linux-x86_64-clang-cross.cmake`.
   Wayland is off in this cross build only, because SDL needs the host tool `wayland-scanner`; X11 is on.

Every target builds this way: `pt`, all unit tests and `pt_setup_linux`. `pt` needs only `libc.so.6`, `libm.so.6` and
`ld-linux-x86-64.so.2` (the C++ runtime is linked in with `-static-libstdc++ -static-libgcc`), and glibc 2.38 or
newer, because the Debian trixie headers map `strtol`, `sscanf` and `fmod` to their 2.38 versions and SDL finds
`strlcpy` there. That is Ubuntu 24.04, Debian 13, Fedora 39 and newer, and current Arch and SteamOS. Building against
an older sysroot would lower it.

## AppImage

`tools/linux/make_appimage.py` turns a Linux build folder into one portable file, `dist/P.T.PC.Port-x86_64.AppImage`
(one per architecture; x86-64 is the only Linux target):

    cmake -G Ninja -B build/linux -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build/linux --parallel 6 --target pt pt_release
    python3 tools/linux/make_appimage.py --build build/linux --exe build/linux/pt_release

Both game targets are built because the font copy hangs off `pt` and the lisa_date copy off `pt_lisa_date_assets` (an
`ALL` target), so `pt_release` alone leaves those two folders out of the build folder.

It fills an AppDir's `usr/bin` with the game (shipped as `pt`), `shaders/`, `fonts/`, `lisa_date/`, `voice/`, the
licences and the upscaler runtimes, all where pt looks for them through `SDL_GetBasePath()`, adds `AppRun`, the desktop
entry and the icon, and hands the folder to appimagetool (`--appimagetool`, `$PT_APPIMAGETOOL`, or `appimagetool` on
PATH). The AppDir stays in `dist/AppDir` to look inside. `.github/workflows/linux.yml` does this on ubuntu-24.04 (whose
glibc 2.39 is newer than the 2.38 the game needs) after the unit tests, and runs the image once before uploading it.

Nothing is written into the image: pt keeps `pt.ini`, saves and `pt.log` in `~/.local/share/pt-port/pt/` (`UserDataDir`
in `src/main.cpp` sees the runtime's `APPIMAGE` variable; `--settings`, `--save-dir` and `--log` still choose another
place). The Konami files are not in the image: `AppRun` passes a `CUSA01127` folder or a single `.zar` that lies next to
the `.AppImage` as `--game`, and `--game`, the first-run screen and `game_dir.txt` work as for every other copy.

The image has no updater of its own: a newer release is a new download (pt.ini and the saves stay in
`~/.local/share/pt-port/pt/`), and the version notice on the PC settings page keeps pointing at the release page and at
the Linux installer, as `docs/updates.md` describes.

The runtime appimagetool writes bundles FUSE 3, so current distributions mount the image as they are; where neither
FUSE 3 nor FUSE 2 is available (a container, a CI runner, an older system) it still runs with
`./P.T.PC.Port-x86_64.AppImage --appimage-extract-and-run`, or with `APPIMAGE_EXTRACT_AND_RUN=1` set, which unpacks it
and runs the copy.

## Running the checks

From a Linux machine or WSL, with the cross build in `build/linux-cross`:

    sh tools/linux/run_tests.sh <game folder> <output folder> <linux installer>

It records the environment (distribution, glibc, Vulkan device, missing libraries with the apt line to install them),
`ldd`, the unit tests (`pt_platform_test --network` covers processes, locks, seek, HTTPS through libcurl and the update
manifest), a 200 frame headless run on the default Vulkan driver and on lavapipe, two scripted routes through
`tools/walkthrough.py`, the Linux installer (self test, update check, install from the game folder, the installed game
started without `--game`), and the Windows build under Proton or Wine when one is installed. The summary is in
`summary.txt`.

## Proton

The Windows build is plain Vulkan with SDL3 and no anti-cheat or launcher, so it runs under Proton or Wine (winevulkan)
as well; FSR, DLSS (through DXVK-NVAPI) and XeSS are Windows DLLs and work there as far as Proton supports them.
`run_tests.sh` runs it when Proton or Wine is present.
