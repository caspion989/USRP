# USRP to GNU Radio — Build Setup Notes

This document records the full environment setup done to get `usrp_to_grc.exe`
building and running on this machine, including every problem hit and how it
was solved.

---

## System State (as of session end)

| Component | Version / Location |
|---|---|
| OS | Windows 11 Home 10.0.26200 |
| Compiler | MSVC 19.44 (VS 2022 Build Tools) |
| CMake | Bundled with VS 2022 Build Tools |
| UHD | 4.10.0.0 at `C:\Program Files\UHD` |
| Boost headers | 1.86.0 extracted to `C:\Users\2406h\boost\` |
| Boost libs | 1.86.0 from Radioconda pkg cache at `C:\Users\2406h\radioconda\pkgs\libboost-1.86.0-hb0986bb_3\Library\lib\` |
| Radioconda | `C:\Users\2406h\radioconda` |
| GNU Radio | Installed via Radioconda |
| Output binary | `build\Release\usrp_to_grc.exe` |

---

## What the Program Does

Connects to a USRP B205mini via UHD, configures it as a receiver, and streams
live IQ samples over UDP (127.0.0.1:12345) to GNU Radio Companion for
real-time visualization.

---

## Problems Encountered and Solutions

### 1. VS Code red squiggle on `#include <uhd/...>` (line 10)

**Problem:** IntelliSense couldn't find UHD headers.  
**Cause:** VS Code doesn't know where UHD is installed.  
**Fix:** Create `.vscode/c_cpp_properties.json` pointing to
`C:/Program Files/UHD/include`. This is cosmetic only — it does not affect the
actual build.

---

### 2. `cmake` not recognized

**Problem:** Running cmake from a regular PowerShell/CMD gave "not recognized".  
**Cause:** cmake is only on PATH inside a Developer Command Prompt.  
**Fix:** `build.bat` calls `vcvars64.bat` at the top, which sets up the full
MSVC + cmake environment automatically.

---

### 3. VS 2022 not at expected path

**Problem:** build.bat initially looked in `C:\Program Files\Microsoft Visual
Studio\2022\...` but VS Build Tools was actually installed at
`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\`.  
**Fix:** Updated build.bat to use the correct `(x86)` path.

---

### 4. `UHDConfig.cmake` missing

**Problem:** CMake's `find_package(UHD)` failed because the UHD install at
`C:\Program Files\UHD` is missing `UHDConfig.cmake` (the CMake support files
were not included in this install).  
**Fix:** Replaced `find_package(UHD)` in CMakeLists.txt with a manually
declared imported target pointing directly at the UHD include and lib paths.

```cmake
set(UHD_ROOT "C:/Program Files/UHD")
add_library(UHD::uhd STATIC IMPORTED)
set_target_properties(UHD::uhd PROPERTIES
    IMPORTED_LOCATION "${UHD_ROOT}/lib/uhd.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${UHD_ROOT}/include;C:/Users/2406h"
)
```

---

### 5. Boost not installed

**Problem:** UHD's own headers internally include Boost (e.g.
`uhd/utils/thread.hpp` includes `boost/thread/thread.hpp`). Boost was not
installed anywhere on the system.  
**Attempts that failed:**
- `conda install -c conda-forge boost-cpp` — environment conflict with existing
  GNURadio packages pinning different Boost versions
- `vcpkg install boost-thread boost-system` — `boost-tokenizer` build failure
- `winget install Boost.Boost` — package not found

**Fix:** Downloaded the official Boost 1.86.0 source zip from
`archives.boost.io` using PowerShell `Invoke-WebRequest`, then extracted just
the `boost/` header folder to `C:\Users\2406h\boost\` using
`System.IO.Compression.ZipFile` in PowerShell. Only the headers (15,828 files)
were extracted — no compilation needed for this step.

The include path added to CMakeLists.txt is `C:/Users/2406h` (the *parent* of
the `boost/` folder, not the folder itself — this is how Boost include paths
always work: `#include <boost/thread.hpp>` requires the parent on the path).

---

### 6. Boost replaced with Winsock2 for UDP

**Problem:** The original `main.cpp` used `boost::asio` for the UDP socket and
`boost::format` for string formatting.  
**Fix:** Both were replaced with standard Windows alternatives:
- `boost::asio` UDP socket → Winsock2 (`winsock2.h`, `Ws2_32.lib`, native to Windows)
- `boost::format` → `std::printf`

This eliminated the need for Boost in *our* code. Boost is still required only
because UHD's headers pull it in internally.

---

### 7. Linker error: `libboost_thread-vc143-mt-x64-1_86.lib` not found

**Problem:** The linker couldn't find `libboost_thread-vc143-mt-x64-1_86.lib`.
UHD uses Boost's Windows auto-linking feature, which embeds `#pragma
comment(lib, ...)` directives with MSVC-tagged library names. Radioconda's
`libboost` package uses simple names like `boost_thread.lib` instead.  
**Fix:**
1. Found that `libboost-1.86.0` libs were already in the Radioconda package
   cache at `C:\Users\2406h\radioconda\pkgs\libboost-1.86.0-hb0986bb_3\Library\lib\`
2. Copied the key libs with the MSVC-tagged names the linker expected:
   ```
   boost_thread.lib  → libboost_thread-vc143-mt-x64-1_86.lib
   boost_system.lib  → libboost_system-vc143-mt-x64-1_86.lib
   boost_chrono.lib  → libboost_chrono-vc143-mt-x64-1_86.lib
   boost_atomic.lib  → libboost_atomic-vc143-mt-x64-1_86.lib
   ```
3. Added that directory to CMakeLists.txt via `target_link_directories`.

---

### 8. "Input line too long" in CMD

**Problem:** Running `build.bat` in an existing terminal gave "The input line
is too long" before vcvars could even run.  
**Cause:** PATH had accumulated entries from multiple previous vcvars calls in
the same terminal session, exceeding CMD's ~8191 character limit.  
**Fix 1 (immediate):** Open a fresh CMD window every time you build.  
**Fix 2 (permanent):** `build.bat` now resets PATH to a minimal set before
calling vcvars:
```bat
set PATH=C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
```

---

## Final File State

### `build.bat`
```bat
@echo off
set PATH=C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cd /d "%~dp0"
if not exist build mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:/Program Files/UHD"
cmake --build . --config Release
```

### `CMakeLists.txt`
```cmake
cmake_minimum_required(VERSION 3.12)
project(usrp_to_grc CXX)

set(CMAKE_CXX_STANDARD 17)

set(UHD_ROOT "C:/Program Files/UHD")
add_library(UHD::uhd STATIC IMPORTED)
set_target_properties(UHD::uhd PROPERTIES
    IMPORTED_LOCATION "${UHD_ROOT}/lib/uhd.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${UHD_ROOT}/include;C:/Users/2406h"
)

set(BOOST_LIB_DIR "C:/Users/2406h/radioconda/pkgs/libboost-1.86.0-hb0986bb_3/Library/lib")
add_executable(usrp_to_grc main.cpp)
target_link_libraries(usrp_to_grc PUBLIC UHD::uhd Ws2_32)
target_link_directories(usrp_to_grc PUBLIC "${BOOST_LIB_DIR}")
```

### `main.cpp` — key changes from original
- Removed `boost/format.hpp` and `boost/asio.hpp`
- Added `winsock2.h` / `ws2tcpip.h` for UDP
- Replaced `boost::format(...)` with `std::printf`
- Replaced `boost::asio` UDP socket with native Winsock2 `sendto`

---

## How to Build (clean)

Open a **new CMD window** (not PowerShell, not an existing CMD that has run
build.bat before):

```cmd
cd C:\Users\2406h\Documents\usrp_hello
rmdir /s /q build
build.bat
```

Output: `build\Release\usrp_to_grc.exe`

---

## How to Run

1. Open **Radioconda Prompt** → launch GNU Radio Companion
2. Build a flowgraph:
   - **UDP Source**: Sample Rate `5e6`, Type `Complex`, IP `127.0.0.1`,
     Port `12345`, Payload Size `8192`
   - **QT GUI Frequency Sink**: Sample Rate `5e6`, wired from UDP Source
3. Click **Run** in GRC (must be running *before* the exe)
4. In CMD:
   ```cmd
   build\Release\usrp_to_grc.exe
   ```
5. Press **Ctrl+C** to stop (sends a clean stream-stop command to the USRP)

---

## Linux Setup (Alternative)

Setting up this project on native Linux is significantly simpler than Windows.
All dependency issues encountered on Windows (missing CMake config, Boost
headers, lib naming mismatches, DLL PATH problems) do not occur on Linux
because the package manager installs everything with matching versions.

### Tested distribution: Ubuntu 22.04 / 24.04

### Step 1 — Install UHD, GNU Radio, and build tools

```bash
sudo apt update
sudo apt install uhd-host libuhd-dev gnuradio cmake g++ git
```

This single command installs:
- UHD runtime + development headers + `UHDConfig.cmake` (no manual CMake wrangling)
- Boost (correct version, headers + libs, already matched to UHD)
- GNU Radio Companion
- CMake and GCC

### Step 2 — Download USRP FPGA images

```bash
sudo uhd_images_downloader
```

Same as Windows. Only needs to be done once.

### Step 3 — Verify the device

Plug in the B205mini and run:

```bash
uhd_usrp_probe
```

Should show `Operating over USB 3` and `Register loopback test passed`.
If you get a permissions error on the USB device, add yourself to the
`plugdev` group:

```bash
sudo usermod -aG plugdev $USER
```

Then log out and back in.

### Step 4 — Restore the original CMakeLists.txt

On Linux, `find_package(UHD)` works correctly, so the CMakeLists.txt can be
restored to its clean original form (no hardcoded paths needed):

```cmake
cmake_minimum_required(VERSION 3.12)
project(usrp_to_grc CXX)

set(CMAKE_CXX_STANDARD 17)

find_package(UHD 4.0.0 REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)

add_executable(usrp_to_grc main.cpp)
target_link_libraries(usrp_to_grc PUBLIC UHD::uhd Boost::system)
```

### Step 5 — Restore the original main.cpp UDP code

On Linux, Boost.Asio is available and works cleanly. The Winsock2 UDP code
in the current `main.cpp` is Windows-only (`winsock2.h`, `SOCKET`, `sendto`).
Replace the socket section with the original Boost.Asio version:

```cpp
#include <boost/asio.hpp>

// setup
boost::asio::io_context io_ctx;
boost::asio::ip::udp::socket socket(io_ctx);
socket.open(boost::asio::ip::udp::v4());
boost::asio::ip::udp::endpoint grc_endpoint(
    boost::asio::ip::make_address(grc_host), grc_port);

// in the loop
socket.send_to(
    boost::asio::buffer(buff.data(),
        num_rx_samps * sizeof(std::complex<float>)),
    grc_endpoint);

// shutdown
socket.close();
```

### Step 6 — Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

Output: `build/usrp_to_grc`

### Step 7 — Run

Same order as Windows — GRC first, then the binary:

```bash
gnuradio-companion &   # or open it from your desktop
./build/usrp_to_grc
```

### Linux vs Windows comparison

| Issue | Windows | Linux |
|---|---|---|
| UHD CMake config | Missing, hardcoded paths | Works out of the box |
| Boost headers | Manual 150 MB download + extract | Installed by apt |
| Boost lib names | Renamed copies needed | Correct names automatically |
| DLL PATH issues | Required copying DLLs to exe dir | No DLLs — shared libs resolved automatically |
| Build command | `build.bat` with vcvars setup | `cmake .. && make` |
| Total setup time | ~2 hours (this session) | ~5 minutes |

---

## Things to Watch Out For

- Always open a **fresh CMD window** before running `build.bat` — reusing the
  same window causes the PATH overflow error
- `uhd.dll` must be on PATH at runtime — `C:\Program Files\UHD\bin` should
  already be on the system PATH from the UHD installer
- The Boost `.lib` files being used are from the Radioconda **package cache**
  (`pkgs/`), not the active environment. If Radioconda is ever reinstalled or
  the cache is cleared, those files will be gone and the build will break. If
  that happens, re-extract them or copy the renamed libs somewhere permanent
- GRC must be started **before** the exe — otherwise UDP packets are sent to
  nothing and the streamer runs blind
