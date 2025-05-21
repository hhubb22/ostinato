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
*   **Qt5:** The Qt5 development libraries. Specifically, `qtbase5-dev`, `qtscript5-dev`, and `libqt5svg5-dev`.
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
  qtscript5-dev \
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
