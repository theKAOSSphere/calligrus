# Building Calligrus

Calligrus is C++20 with CMake. CMake downloads and builds the dependencies itself (Dear ImGui, GLFW, md4c, stb,
libwebp, toml++, zlib, HarfBuzz, FreeType): `cmake/deps.cmake` pins each one to a release tarball with a SHA-256, and
they are linked in statically. Fonts, themes, the demo document and the syntax grammars are compiled into the
executable, so the result is a single file. The first configure needs network access; the downloaded archives are kept
in `<build folder>/_downloads` (set the environment variable or CMake variable `CALLIGRUS_DOWNLOAD_DIR` to share them
between build folders; CI caches that folder).

A plain build needs nothing but CMake and a compiler:

```sh
cmake -S . -B build && cmake --build build --config Release
```

To build against the system's libraries instead (experimental: not tested in CI; for distribution packagers), configure with
`-DCALLIGRUS_USE_SYSTEM_LIBS=ON`. CMake then needs pkg-config and the development packages of md4c, libwebp, toml++,
HarfBuzz (with the subsetter), GLFW 3, zlib, FreeType and stb. Dear ImGui is still downloaded, because it is built with
options (FreeType, 32-bit characters) that distributions do not ship.

## Windows

Needs Visual Studio 2022 (or the Build Tools) with the C++ workload, and CMake.

```powershell
.\build.ps1                      # Release build in build\ ; output build\Release\calligrus.exe
.\build.ps1 -Config Debug        # or: -BuildDir build-debug
```

Every library and the C/C++ runtime (`/MT`) are linked in, so `calligrus.exe` needs no DLLs.

## Linux

Needs a C/C++ compiler, CMake, Ninja (or make) and the X11 / Wayland / OpenGL development packages GLFW builds
against. On Ubuntu:

```sh
sudo apt install build-essential cmake ninja-build pkg-config \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev \
    libxkbcommon-dev libwayland-dev wayland-protocols libxext-dev
./build.sh                       # Release build in build/ ; output build/calligrus
./build.sh Release --test        # also runs every build/calligrus-*-test
```

`BUILD_DIR=/some/other/dir ./build.sh` builds outside the source tree; `CALLIGRUS_USE_SYSTEM_LIBS=ON ./build.sh` uses
the distribution's libraries (experimental: not tested in CI).

## Android

Calligrus also runs on Android (a spike: see spec section 35). The app is the same C++ as the desktop app, loaded by
GameActivity; its Java side is in `android/` (a Gradle project). Needs a Linux machine (or WSL) with:

- the Android SDK (platform 36, build-tools 35 or 36, CMake 3.22.1) and NDK 27.2.12479018 (`ANDROID_HOME`; the NDK is
  found as `$ANDROID_HOME/ndk/27.2.12479018`), and JDK 17 for Gradle;
- network access for the first build: Gradle's CMake downloads and builds the libraries for each CPU with the NDK
  toolchain, exactly as on the desktop (`cmake/deps.cmake`). Set `CALLIGRUS_DOWNLOAD_DIR` to keep the archives outside
  the build folders.

```sh
export ANDROID_HOME=~/Android/Sdk ANDROID_NDK_HOME=~/Android/Sdk/ndk/27.2.12479018
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
cd android
sh gradlew assembleDebug                          # app/build/outputs/apk/debug/app-debug.apk (arm64-v8a + x86_64)
sh gradlew assembleDebug -PcalligrusAbis=x86_64   # one CPU only (faster)
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

The first build compiles the libraries (a few minutes per CPU); later ones reuse them. Run in the emulator
headless with `emulator -avd NAME -no-window -no-audio -gpu swiftshader_indirect` (KVM makes it usable).

### Android release build

`assembleRelease` needs no credentials and gives an unsigned, optimised APK (native Release, symbols stripped, R8 on;
arm64-v8a only unless `-PcalligrusAbis=arm64-v8a,x86_64`). `versionName` is read from `project(calligrus VERSION ...)`
in `CMakeLists.txt`; `versionCode` is in `android/app/build.gradle.kts`.

```sh
cd android
sh gradlew assembleRelease     # app/build/outputs/apk/release/app-release-unsigned.apk
```

An unsigned APK does not install. Sign it on the machine that holds your keystore (Windows, PowerShell; needs the
Android SDK build-tools, found through `ANDROID_HOME` / `ANDROID_SDK_ROOT` / `C:\tools\android-sdk`):

```powershell
.\android\sign-release.ps1 -Apk calligrus-0.1.0-unsigned.apk
# -Out (default: <name>-signed.apk), -Keystore (default %USERPROFILE%\.kaoss\kaoss-release.jks), -Alias (default calligrus)
```

The script runs `zipalign` and `apksigner`; apksigner asks for the password itself and the script then prints the
certificate. Keep the keystore out of the repository.

For CI, Gradle signs the release itself when all of these environment variables are set (nothing is read from files in
the tree): `CALLIGRUS_RELEASE_KEYSTORE` (path), `CALLIGRUS_RELEASE_STORE_PASSWORD`, optionally
`CALLIGRUS_RELEASE_KEY_ALIAS` (default `calligrus`) and `CALLIGRUS_RELEASE_KEY_PASSWORD` (default: the store password).
Otherwise the APK stays unsigned.

GitHub Actions does this with two repository secrets: `CALLIGRUS_KEYSTORE_B64` (the keystore, base64) and
`CALLIGRUS_KEYSTORE_PASSWORD` (store and key password; the alias is `calligrus`). The CI `android` job builds a signed
APK when they exist (artifact `calligrus-android`) and an unsigned one otherwise (fork pull requests). The release
workflow requires them and attaches `calligrus-<tag>-android-arm64.apk` to the draft release.

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
