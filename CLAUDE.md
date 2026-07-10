# CLAUDE.md


dont act if you dont understand what im asking you to do and dont push or pull from git without my explicit command 

This file gives Claude (and future-you) context on this project.

## What this project is

A C++ program that connects to an Ettus USRP B205mini via UHD, configures it
as a receiver, receives live IQ samples, and runs a **real-time FFT** on them
to find the frequency of the strongest signal in the band. Based on the
configuration example at
https://kb.ettus.com/Getting_Started_with_UHD_and_C%2B%2B, extended with a
threaded receive loop and a self-contained FFT.

The program does NOT forward samples anywhere. It uses a **producer/consumer**
design: the main thread receives IQ samples and hands batches to a second
(DSP) thread over a bounded queue; the DSP thread runs an FFT and prints the
peak frequency. See "Current state of the script" below for details.

## Files

- `main.cpp` — the whole program (see "Current state of the script"). Producer
  loop + FFT consumer thread + a self-contained radix-2 FFT.
- `CMakeLists.txt` — build config. Depends only on UHD (hardcoded paths —
  `UHDConfig.cmake` is missing from this install). No Boost or Winsock2.
- `build.bat` — resets PATH to minimal, calls vcvars64.bat, then runs cmake.
- `SETUP_NOTES.md` — full record of every setup problem and solution.
- `README.md` — project overview and architecture diagram.
- `UHD_REFERENCE.md` — line-by-line explanation of every UHD call in main.cpp,
  in required order, plus device-specific vs universal API notes.
- `CPP_PRIMITIVES.md` — reference for the C++ concurrency/language primitives
  used (atomics, mutex, condition_variable, lambdas, `std::ref`, containers).

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

## Current state of the script

`main.cpp` is a threaded producer/consumer that does real-time FFT spectral
analysis. Flow:

```
main thread (producer)                    DSP thread (consumer: fft())
──────────────────────                    ────────────────────────────
recv() 4096 samples into recv_buf         wait on queue
copy into a fresh `batch` vector          pop a batch
push batch onto SampleQueue        ──►     zero-pad 4096 → 16384 points
(blocks if queue depth ≥ MAX_DEPTH)        fft_inplace() on the padded copy
                                           find peak bin (skips DC / bin 0)
                                           bin → frequency, print ~1×/sec
```

Key pieces, top to bottom in the file:

- **`stop_signal_called`** — `std::atomic<bool>` set by the Ctrl+C handler;
  both threads poll it to shut down cleanly.
- **`SampleQueue`** — `std::queue<std::vector<std::complex<float>>>` + mutex +
  condition_variable. `MAX_DEPTH = 32` batches of back-pressure: the producer
  blocks rather than letting the queue grow unbounded.
- **`fft_inplace()`** — self-contained iterative radix-2 Cooley-Tukey FFT
  (bit-reversal + butterflies). No external library. Input length MUST be a
  power of 2. Transforms in place, destroying its input. Uses `std::acos(-1.0)`
  for π (MSVC does not define `M_PI` without `_USE_MATH_DEFINES`).
- **`fft()`** — the consumer thread. Pops a batch, copies it into a
  pre-allocated `padded` buffer (zeros in the tail), FFTs the copy so the
  original `batch` samples survive, finds the strongest bin **skipping bin 0**
  (the B205mini is direct-conversion → a DC/LO-leakage spike sits at the tuned
  center frequency), converts the bin to an absolute RF frequency
  (`center_freq + bin·rate/N`), and prints it **throttled to ~1 Hz** (the FFT
  runs ~1200×/s; printing every result would stall the pipeline).
- **`UHD_SAFE_MAIN`** — configures the USRP, starts the DSP thread, runs the
  producer recv loop, then on Ctrl+C issues STREAM_MODE_STOP, wakes and
  `join()`s the DSP thread.

Current hardcoded DSP values:
- `samples_per_packet = 4096` (real samples per recv, a power of 2).
- `ZERO_PAD_FACTOR = 4` → `fft_size = 16384`. Zero-padding gives finer bin
  spacing (`rate/16384 ≈ 305 Hz`) but does NOT improve true resolution
  (`rate/4096 ≈ 1221 Hz`, fixed by the real sample count / observation time).
  The startup print labels both honestly.

Note: an earlier version wrote samples to `samples.csv` from the DSP thread;
that CSV writer has been replaced by the FFT. Some reference docs
(`CPP_PRIMITIVES.md`, `UHD_REFERENCE.md`) still cite the `csv_writer` example
when explaining the producer/consumer pattern — the mechanism is identical,
only the consumer's work changed.

### Short reads: the current FFT drops odd-sized frames

The FFT thread has a guard: `if (batch.size() != samples_per_packet) continue;`
A radix-2 FFT needs a fixed power-of-2 length, so any batch that isn't exactly
4096 samples (a rare short `recv()` read — startup, post-overflow, timeout) is
skipped. This is a skipped *analysis frame*, not hardware sample loss — the
producer still received those samples; the FFT just declines to transform an
odd length. In steady streaming `recv()` almost always returns full buffers, so
this is rare. Options to avoid it entirely (accumulate a window / separate the
sample-action / zero-pad the short batch) are written up in `THREADING_NOTES.md`
§18.

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
