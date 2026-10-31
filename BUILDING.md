# Building Calligrus

Calligrus is C++20 with CMake. Dependencies (Dear ImGui, GLFW, md4c, stb, libwebp, toml++, zlib, HarfBuzz) come
from [vcpkg](https://github.com/microsoft/vcpkg) in manifest mode (`vcpkg.json`). Fonts, themes, the demo document
and the syntax grammars are compiled into the executable, so the result is a single file.

## Windows

Needs Visual Studio 2022 (or the Build Tools) with the C++ workload, CMake, and vcpkg. Set `VCPKG_ROOT` to your
vcpkg folder (or put vcpkg in `C:\tools\vcpkg`, `C:\vcpkg`, `C:\src\vcpkg` or `%USERPROFILE%\vcpkg`).

```powershell
.\build.ps1                      # Release build in build\ ; output build\Release\calligrus.exe
.\build.ps1 -Config Debug        # or: -BuildDir build-debug
```

`build.ps1` uses the `x64-windows-static` triplet: every library and the C/C++ runtime are linked in, so
`calligrus.exe` needs no DLLs.

## Linux

Needs a C/C++ compiler, CMake, Ninja (or make), git, curl, zip, unzip, tar, pkg-config and the X11 / OpenGL
development packages GLFW builds against. On Ubuntu:

```sh
sudo apt install build-essential cmake ninja-build git curl zip unzip tar pkg-config autoconf autoconf-archive automake libtool \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev \
    libxkbcommon-dev libwayland-dev libxext-dev
./build.sh                       # Release build in build/ ; output build/calligrus
./build.sh Release --test        # also runs every build/calligrus-*-test
```

`build.sh` finds vcpkg in `$VCPKG_ROOT`, `~/vcpkg`, `/opt/vcpkg` or `/usr/local/vcpkg`, and clones it into `~/vcpkg`
if it is missing. `BUILD_DIR=/some/other/dir ./build.sh` builds outside the source tree.

## Tests

Every `calligrus-*-test` executable in the build folder is a console test with exit code 0 on success (no window).

The visual suite renders about 130 screenshots headless and compares them with `spec/tests/visual/refs/<os>/`:

```sh
cd spec/tests/visual
python run.py                    # needs Python 3 with Pillow; PyMuPDF is optional (PDF text checks)
python run.py --update           # accept the current output as the new references
```

On Linux it runs under `xvfb-run` (install `xvfb`), with `DISPLAY` and `WAYLAND_DISPLAY` removed so no window opens.
`scripts/check-linux.sh` builds, runs every test and the visual suite in one go. See `spec/tests/visual/README.md`.

## Editor plugins

`editors/emacs/calligrus.el` and `editors/nvim` drive `calligrus --editor` (live preview, cursor sync, Ctrl+click back
to the source). See the README in each folder; the Emacs tests run with
`emacs --batch -l editors/emacs/test/calligrus-test.el -f ert-run-tests-batch-and-exit`.
