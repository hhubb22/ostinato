# Building Ostinato with CMake

CMake is the supported build system. qmake cannot provide the pinned QuickJS
dependency consistently across supported platforms and is now explicitly
unsupported; the root `ost.pro` stops with an error instead of producing a
partially linked build.

Ostinato currently requires a C++11 compiler, Qt 5.7 or newer (including the
Core, Widgets, Network, Xml, Svg, and Test development components), Protocol
Buffers, and libpcap. Linux builds additionally require libnl3 and
libnl-route-3. User scripts use QuickJS-NG 0.15.1; QtScript is not required.

## Linux

On Debian/Ubuntu, install the build dependencies with:

```bash
sudo apt-get update
sudo apt-get install cmake ninja-build g++ \
  qtbase5-dev libqt5svg5-dev \
  libpcap-dev libprotobuf-dev protobuf-compiler \
  libnl-3-dev libnl-route-3-dev pkg-config
```

On Fedora, use:

```bash
sudo dnf install cmake ninja-build gcc-c++ \
  qt5-qtbase-devel qt5-qtsvg-devel \
  libpcap-devel protobuf-devel protobuf-compiler libnl3-devel pkgconf-pkg-config
```

Configure and build an out-of-tree Debug build:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

### Docker build environment

The public `linux/amd64` image contains the complete Ubuntu build toolchain and
Linux development dependencies. Pull it from GitHub Container Registry:

```bash
docker pull ghcr.io/hhubb22/ostinato-build:latest
```

Then mount the source tree and build as the current user. Setting `HOME` to a
writable directory lets tools that consult it work when the host user does not
exist in the container's `/etc/passwd`:

```bash
docker run --rm \
  --user "$(id -u):$(id -g)" \
  -e HOME=/tmp \
  -v "$PWD:/workspace" \
  ghcr.io/hhubb22/ostinato-build:latest \
  bash -c 'cmake -S . -B build-docker -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON && \
    cmake --build build-docker --parallel && \
    ctest --test-dir build-docker --output-on-failure'
```

The repository also includes the image's Dockerfile. To rebuild it locally
instead of pulling the published image, run:

```bash
docker build -t ostinato-build .
```

Use `ostinato-build` in the `docker run` command above when using the locally
built image.

Build output is written to `build-docker` in the source tree and remains owned
by the current host user. QuickJS is downloaded and verified by CMake during
the first configuration, so the container needs network access for that step.
For an offline container build, use the `FETCHCONTENT_SOURCE_DIR_QUICKJS`
option described below and mount the extracted source directory into the
container.

Use `-DCMAKE_BUILD_TYPE=Release` for a Release build. The resulting executables
are `<build-dir>/client/ostinato` and `<build-dir>/server/drone` (`build` in the
host example or `build-docker` in the Docker example). Grant `drone` the
required packet and network administration capabilities before running it:

```bash
sudo setcap cap_net_raw,cap_net_admin=eip build/server/drone
```

`BUILD_TESTING` defaults to `ON`; pass `-DBUILD_TESTING=OFF` to omit automated
test targets. The tests are deterministic and require neither packet-capture
permissions nor a GUI display. The legacy `test/test.pro` PCAP import utility is
available as the manual-only `ostinato_importpcap` target and is not registered
with CTest:

```bash
cmake --build build --target ostinato_importpcap
build/test/ostinato_importpcap importpcap capture.pcap
```

## QuickJS dependency and offline builds

CMake uses QuickJS-NG 0.15.1 commit
`fd0a0210b7be00957751871e7e01b8291268fc29`, whose upstream CMake supports
Linux, MSVC, and macOS. FetchContent downloads that immutable revision archive
and verifies SHA-256
`39d931489e99f80b496f900f88d67d7aad6b5f25ce83810a408eb42f6af6839e`.
Only the core engine target is part of the Ostinato build. The archive includes
QuickJS-NG's MIT license. Fedora 43 does not currently package QuickJS, so the
verified FetchContent path is the normal Fedora configuration.

For a fully offline build, download and extract that exact archive in advance,
then point FetchContent at it (the directory must contain `CMakeLists.txt`,
`quickjs.c`, and `quickjs.h`):

```bash
cmake -S . -B build -G Ninja \
  -DFETCHCONTENT_SOURCE_DIR_QUICKJS=/path/to/quickjs-ng-0.15.1 \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
```

QuickJS does not have a stable C ABI, so arbitrary system headers and libraries
are not accepted. Use the pinned source above for both online and offline
builds. The QuickJS target is always static, including when Ostinato is
configured with `BUILD_SHARED_LIBS=ON`.

Install with the normal CMake prefix controls (the default Unix prefix is
`/usr/local`):

```bash
cmake --install build
# or: cmake --install build --prefix /custom/prefix
```

The controller themes are installed under
`share/ostinato-controller/themes`, matching the runtime lookup behavior.

## Windows

Install a Qt 5 MSVC kit and Protobuf development package (for example through
vcpkg), and install the Npcap SDK. Configure from a matching Visual Studio
developer prompt, passing the package locations as needed:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH=C:\Qt\5.15.2\msvc2019_64 `
  -DNPCAP_ROOT=C:\npcap-sdk
cmake --build build --config Release
```

`NPCAP_ROOT` must contain the Npcap SDK `Include` and `Lib` directories. The
build links `wpcap`, `Packet`, and `iphlpapi`, and keeps the existing Vista-or-
newer API definitions. Windows builds have not been validated as part of this
initial migration.

## macOS and BSD

Install Qt 5, Protobuf, and libpcap, make them discoverable by CMake, then use
the same `cmake -S` and `cmake --build` workflow. On macOS the controller is an
`Ostinato.app` bundle and themes install into its `Contents/SharedSupport`
directory. macOS and BSD builds have not been validated as part of this initial
migration.

## Build-system notes

- All `.proto` files are compiled from the source tree into the build tree;
  pre-generated protobuf sources are neither required nor used.
- Qt MOC, UIC, and RCC processing is automatic.
- Debug builds retain the qmake build's strict GCC/Clang warnings. These flags
  are not passed to MSVC, and generated protobuf sources are exempt from
  `-Werror` as before.
- The application revision is captured from Git when CMake configures the
  build. Re-run CMake after changing revisions if the displayed revision must
  be refreshed; qmake previously regenerated this file during compilation.

# Historical qmake instructions (unsupported)

The material below is retained only as historical platform setup context.
qmake builds are no longer supported and `qmake ost.pro` intentionally fails;
use the CMake workflow above.

# General Notes

This document provides instructions for building the project on various platforms. It is recommended to read this section before proceeding to platform-specific instructions.

## Cloning the Repository

1.  **Get the Repository URL:** Obtain the clone URL for the project repository (e.g., from GitHub, GitLab, or another source). It will look something like `https://github.com/user/project.git` or `git@github.com:user/project.git`. Let's call this `<repository-url>`.

2.  **Clone:**
    To clone the repository, open a terminal or command prompt and use the following command. Replace `<repository-url>` with the actual URL.
    ```bash
    git clone --recursive <repository-url>
    ```
    This will create a new directory, typically named after the project (e.g., `project`). Let's call this `<repository-directory>`. Navigate into it:
    ```bash
    cd <repository-directory>
    ```
    The `--recursive` flag is included as good practice to automatically initialize and update any git submodules the project might use.

3.  **Submodules Note:**
    As of the last update to this document, this project does not utilize git submodules. However, if they were added in the future and you initially cloned without `--recursive`, you could initialize and update them using:
    ```bash
    git submodule init
    git submodule update
    ```

## Network Privileges for `drone` Executable

The `drone` executable, a core component of this project, requires special network privileges to perform tasks like packet sniffing and raw socket manipulation. The executable is typically found in a build output directory (e.g., `build/`, `bin/`, or specific to your build configuration) after successful compilation.

*   **On Linux:** You will need to grant capabilities to the executable.
    ```bash
    sudo setcap cap_net_raw,cap_net_admin=eip path/to/your/drone
    ```
    Replace `path/to/your/drone` with the actual path to the compiled `drone` executable (e.g., `build/drone` or `release/drone`).

*   **On Windows:** The `drone` executable will likely require administrator privileges.
    *   Right-click the `drone.exe` executable and select "Run as administrator".
    *   Ensure Npcap was installed with "WinPcap API-compatible Mode" (as detailed in the Windows build section).

Without these elevated privileges, the `drone` executable may not operate as expected, particularly for features involving direct network interface access.

---

# Building on Linux

This guide provides instructions for compiling the project on a Linux system, primarily targeting Debian/Ubuntu distributions.

## Prerequisites

Before you begin, ensure you have the following dependencies installed:

*   **C++ Compiler:** A modern C++ compiler that supports C++11 or later (e.g., GCC, Clang).
*   **Qt5:** The Qt5 development libraries. Specifically, `qtbase5-dev` and `libqt5svg5-dev`.
*   **libpcap:** The library for network packet capture (`libpcap-dev`).
*   **Protocol Buffers:** Google's data interchange format (`libprotobuf-dev`, `protobuf-compiler`).
*   **libnl3:** The Netlink Protocol Library Suite, version 3 (`libnl-3-dev`, `libnl-route-3-dev`).
*   **make:** The build automation tool.

## Dependency Installation (Debian/Ubuntu)

You can install the required dependencies on a Debian/Ubuntu system using the following command:

```bash
sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends \
  g++ \
  qtbase5-dev \
  libqt5svg5-dev \
  libpcap-dev \
  libprotobuf-dev \
  protobuf-compiler \
  libnl-3-dev \
  libnl-route-3-dev \
  make
```
**Note:** You can replace `g++` with `clang++` if you prefer to use Clang.

## Build Instructions

1.  **Ensure you are in the repository directory:** If you haven't already, `cd <repository-directory>` as described in the "General Notes" under "Cloning the Repository".

2.  **Configure the build using qmake:**
    ```bash
    QT_SELECT=qt5 qmake -config debug
    ```
    Alternatively, if `qt5` is your system default, you might be able to just run:
    ```bash
    qmake -config debug
    ```
    For a release build, you might use `qmake -config release`.

3.  **Compile the project using make:**
    ```bash
    make
    ```
    To use a specific compiler (e.g., clang++ if you installed clang and want to override the default or qmake's choice):
    ```bash
    make CXX=clang++
    ```

## Post-Build Steps

Refer to the "Network Privileges for `drone` Executable" section in "General Notes" for instructions on granting necessary permissions to the compiled `drone` executable.

---

This guide is based on the build process defined in `.github/workflows/build-linux.yml`. If you encounter issues, refer to that workflow file for potentially more up-to-date build steps.

---

# Building on Windows

This guide provides instructions for compiling the project on a Windows system.

## Prerequisites

Before you begin, ensure you have the following installed:

*   **Visual Studio:** A recent version of Visual Studio (e.g., 2019 or newer) with the "Desktop development with C++" workload installed. This provides the MSVC C++ compiler.
*   **Qt5 Development Libraries:** Download and install the Qt5 libraries for your MSVC version (e.g., `msvc2019_64` for VS2019 64-bit). Get this from the official Qt website, ensuring the installed Qt MSVC version matches your Visual Studio compiler version and architecture.
*   **Npcap:** The Npcap library for packet capture. Download the installer from the official Npcap website. **Crucially, during installation, check the option "Install Npcap in WinPcap API-compatible Mode".**
*   **Protocol Buffers (protobuf):**
    *   **Compiler (`protoc.exe`):** Download the precompiled Windows `protoc` executable (e.g., `protoc-xxx-win64.zip`) from the Protocol Buffers GitHub releases page. Extract `protoc.exe` to a chosen directory (e.g., `C:\Program Files\protoc\bin`). Add this directory to your system's `PATH` environment variable.
    *   **Libraries:** You will need the Protocol Buffers C++ runtime libraries. Options include:
        *   Using a package manager like `vcpkg`:
          ```bash
          vcpkg install protobuf protobuf:x64-windows # Or protobuf:x86-windows for 32-bit
          ```
          Ensure `vcpkg` is integrated with your Visual Studio environment.
        *   Compiling from source using Visual Studio (refer to official protobuf documentation).

## Dependency Installation Summary

*   **Visual Studio:** Installer from Microsoft's Visual Studio website.
*   **Qt5:** Installer from the official Qt website (select the correct MSVC version).
*   **Npcap:** Installer from the Npcap website.
*   **Protocol Buffers:** `protoc.exe` from GitHub releases; libraries via `vcpkg` or source compilation.

## Build Instructions

1.  **Ensure you are in the repository directory:** If you haven't already, `cd <repository-directory>` as described in the "General Notes" under "Cloning the Repository".

2.  **Set up the Build Environment:**
    Open a "Developer Command Prompt for VS" (e.g., "x64 Native Tools Command Prompt for VS 2019") that matches the architecture you are building for. This pre-configures the environment for the MSVC compiler and tools like `nmake`.
    Ensure your Qt `bin` directory (e.g., `C:\Qt\5.15.2\msvc2019_64\bin`) and the directory containing `protoc.exe` are in your `PATH` environment variable for this command prompt session (or set system-wide).

3.  **Configure the build using qmake:**
    In the Developer Command Prompt, navigate to the project's root directory (`<repository-directory>`).
    ```bash
    qmake -config release  # Or 'debug' for a debug build
    ```
    If you have multiple Qt versions or kits, you might need to explicitly qualify the path to `qmake.exe` or ensure the correct one is found first via the `PATH`.

4.  **Compile the project:**
    Use `nmake` (part of Visual Studio) to build:
    ```bash
    nmake
    ```
    Alternatively, for potentially faster builds on multi-core systems, consider `jom`:
    ```bash
    jom
    ```
    (`jom.exe` needs to be downloaded separately and its location added to your `PATH`).

## Post-Build Steps

Refer to the "Network Privileges for `drone` Executable" section in "General Notes" for instructions on running the compiled `drone.exe` with appropriate permissions.

## Configuration Notes & Troubleshooting

*   **PATH Environment Variable:** Correct `PATH` configuration is critical. Double-check that it includes:
    *   Your Qt version's `bin` directory (e.g., `C:\Qt\Qt5.15.2\msvc2019_64\bin`).
    *   The directory containing `protoc.exe`.
    *   If using `jom`, the directory containing `jom.exe`.
*   **Npcap Installation:** Re-iterate: Npcap must be installed with "WinPcap API-compatible Mode".
*   **Protocol Buffer Files (`.proto`):** If the project uses `.proto` files, `qmake` (via the project's `.pro` file) should ideally be configured to invoke `protoc.exe` automatically. If you encounter errors related to missing generated protobuf headers (e.g., `*.pb.h`), you might need to run `protoc.exe` manually or troubleshoot the qmake/project configuration.
*   **MSVC and Qt Version Mismatch:** Ensure the Qt libraries are for the same MSVC version and architecture (32-bit/64-bit) as your Visual Studio compiler.

This guide provides general steps. Specific project configurations might require adjustments. Refer to any project-specific README files or scripts if available.
