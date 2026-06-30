# CLAUDE.md


dont act if you dont understand what im asking you to do and dont push or pull from git without my explicit command 

This file gives Claude (and future-you) context on this project.

## What this project is

A C++ program that connects to an Ettus USRP B205mini via UHD, configures it
as a receiver, and receives live IQ samples into a buffer for custom signal
processing. Based on the configuration example at
https://kb.ettus.com/Getting_Started_with_UHD_and_C%2B%2B, extended with an
actual rx_streamer receive loop.

The program does NOT forward samples anywhere — the receive loop captures raw
IQ samples into `buff[]` and exposes them for processing inside the loop.

## Files

- `main.cpp` — connects to the USRP, configures rate/freq/gain/bandwidth/
  antenna, opens an RX streamer, and receives IQ samples into `buff[]` as
  `std::complex<float>`. Processing logic goes inside the receive loop.
- `CMakeLists.txt` — build config. Depends only on UHD (hardcoded paths —
  `UHDConfig.cmake` is missing from this install). No Boost or Winsock2.
- `build.bat` — resets PATH to minimal, calls vcvars64.bat, then runs cmake.
- `SETUP_NOTES.md` — full record of every setup problem and solution.
- `README.md` — project overview and architecture diagram.

## Environment / dependencies (Windows)

- **UHD 4.10.0.0** installed at `C:\Program Files\UHD`, with
  `C:\Program Files\UHD\bin` on PATH (needed for `uhd.dll` and `libusb-1.0.dll`).
- **Boost 1.86.0 headers** extracted to `C:\Users\2406h\boost\` (header-only,
  needed because UHD headers internally include Boost).
- **Boost 1.86.0 libs** from Radioconda pkg cache at
  `C:\Users\2406h\radioconda\pkgs\libboost-1.86.0-hb0986bb_3\Library\lib\`
  (needed because UHD auto-links Boost via `#pragma comment(lib, ...)`).
- **MSVC 19.44** (VS 2022 Build Tools at
  `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\`).
- Hardware: **USRP B205mini**, USB 3.0 confirmed working
  (`uhd_usrp_probe` reports "Operating over USB 3" and "Register loopback
  test passed").

## Build

Open a **fresh CMD window** and run:

```cmd
cd C:\Users\2406h\Documents\usrp_hello
build.bat
```

Output: `build\Release\usrp_to_grc.exe`

Never reuse the same CMD window across build sessions — PATH accumulates and
hits the 8191 character limit. `build.bat` resets PATH at the top to prevent
this, but only works correctly in a fresh window.

## Run

```cmd
build\Release\usrp_to_grc.exe
```

Press Ctrl+C to stop cleanly (issues a stream-stop command to the USRP FPGA).

## Architecture

```
Antenna (RX2) → AD9361 RF chip → FPGA → USB 3.0 → uhd.dll → recv() → buff[]
```

All logic runs on the PC. The B205mini is a USB peripheral with no on-board CPU.

## Hardware-specific values in main.cpp

These are specific to the B205mini and should NOT be changed without checking
`uhd_usrp_probe` output first:
- `device_args = "type=b200"` — B205mini reports as device type `b200`.
- `subdev = "A:A"` — B205mini has a single RX/TX frontend.
- `ant = "RX2"` — antenna must be physically connected to this port.
  (`"TX/RX"` is the other valid option, used for transmit.)

Tunable per use case (hardcoded near the top of `main()`):
`rate` (Msps), `freq` (Hz), `gain` (dB), `bw` (Hz).

## Known gotchas

- Always open a **new** CMD window before running `build.bat`.
- `uhd.dll` and `libusb-1.0.dll` are copied into `build\Release\` so the exe
  finds them regardless of PATH at runtime.
- The Boost `.lib` files are from the Radioconda **package cache** (`pkgs/`).
  If Radioconda is reinstalled or the cache is cleared, the build will break —
  copy the renamed libs somewhere permanent if that happens.
- "Operating over USB 2" in `uhd_usrp_probe` → wrong cable or port, caps at
  ~8 MS/s. Confirm USB 3 before using high sample rates.
- FPGA compatibility number mismatch after a UHD upgrade → re-run
  `uhd_images_downloader`, then unplug/replug the device.
