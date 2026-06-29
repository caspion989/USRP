# CLAUDE.md

This file gives Claude (and future-you) context on this project. Place it in
the same folder as `main.cpp` and `CMakeLists.txt`.

## What this project is

A C++ program that connects to an Ettus USRP B205mini via UHD, configures it
as a receiver, and streams live IQ samples over UDP to GNU Radio Companion
(GRC) for real-time visualization. Based on the configuration example at
https://kb.ettus.com/Getting_Started_with_UHD_and_C%2B%2B, extended with an
actual rx_streamer receive loop and UDP forwarding (the KB page only covers
configuring the device, not streaming or talking to GNU Radio).

## Files

- `main.cpp` — connects to the USRP, configures rate/freq/gain/bandwidth/
  antenna, opens an RX streamer, and forwards received samples to GRC over
  UDP (127.0.0.1:12345 by default) as raw `std::complex<float>` (matches
  GNU Radio's "complex" stream type exactly — no conversion needed).
- `CMakeLists.txt` — build config. Depends on UHD (`find_package(UHD 4.10.0
  REQUIRED)`) and Boost (`Boost::system`, for Boost.Asio's UDP socket).

## Environment / dependencies (Windows)

- **UHD 4.10.0.0** installed system-wide at `C:\Program Files\UHD`, with
  `C:\Program Files\UHD\bin` on PATH (needed for `uhd.dll`, `libusb-1.0.dll`,
  and the `uhd_*` command-line tools).
- **MSVC build tools** (Visual Studio 2022 "Desktop development with C++"
  workload, or the standalone Build Tools installer — the full Visual Studio
  IDE itself is never opened; only its compiler/linker are used).
- **VS Code** with the CMake Tools + C/C++ extensions, launched from a
  Developer Command Prompt/PowerShell so MSVC is on PATH.
- **GNU Radio Companion**, installed via Radioconda. Note: Radioconda bundles
  its *own* separate copy of UHD inside its conda environment — run
  `uhd_find_devices` / `uhd_images_downloader` from the "Radioconda Prompt"
  too if GRC has trouble seeing the device.
- Hardware: **USRP B205mini**, USB 3.0 connection confirmed working
  (`uhd_usrp_probe` reports "Operating over USB 3" and "Register loopback
  test passed").

## Build

```
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```
(`-G "Visual Studio 17 2022"` selects the MSVC toolset via CMake's generator
name — it does not open the Visual Studio IDE.)

If CMake can't find UHD, add `-DCMAKE_PREFIX_PATH="C:/Program Files/UHD"`.

## Run

1. Open GNU Radio Companion. Build/open a flowgraph with:
   - **UDP Source**: Sample Rate `5e6`, Type `Complex`, IP `127.0.0.1`,
     Port `12345`, Payload Size `8192` (must be ≥ the C++ program's packet
     size, currently 1024 samples × 8 bytes = 8192 bytes).
   - **QT GUI Frequency Sink** (or QT GUI Sink), Sample Rate `5e6`, wired
     directly from the UDP Source. No Throttle block — the UDP Source is
     already paced by real incoming samples.
2. **Execute the GRC flowgraph first** (so the UDP listener is bound before
   anything sends to it).
3. Run `build\Release\usrp_to_grc.exe`.
4. Ctrl+C in the console to stop cleanly (issues a proper stream-stop to the
   USRP instead of just killing the process).

## Hardware-specific values in main.cpp

These are specific to the B205mini and should NOT be changed to match other
USRP models without checking `uhd_usrp_probe` output first:
- `device_args = "type=b200"` — B205mini reports as device type `b200`.
- `subdev = "A:A"` — B205mini has a single RX/TX frontend.
- `ant = "RX2"` — antenna must be physically connected to this port to
  match. (`"TX/RX"` is the other valid option, used for transmit.)

Tunable per use case (currently hardcoded near the top of `main()`):
`rate` (Msps), `freq` (Hz), `gain` (dB), `bw` (Hz).

## Known gotchas from getting this working

- Always open a **new** terminal after changing PATH or reinstalling UHD —
  old terminal windows keep a stale PATH.
- "libusb-1.0 was not found" → check `C:\Program Files\UHD\bin\
  libusb-1.0.dll` exists; if missing, reinstall UHD via the official
  installer rather than sourcing the DLL separately.
- "Operating over USB 2" in `uhd_usrp_probe` output → wrong cable/port;
  caps you around 8 MS/s. Confirm "Operating over USB 3" before relying on
  higher sample rates.
- UHD ≥ 4.9 auto-installs the WinUSB driver on install — the old manual
  `erllc_uhd`/Zadig driver process is not needed on current UHD.
- FPGA "compatibility number" mismatch after a UHD upgrade → re-run
  `uhd_images_downloader`, then unplug/replug the device.
