# USRP IQ Receiver

A C++ program that connects to an **Ettus USRP B205mini** via the UHD library,
configures it as a receiver, and streams live IQ samples into a buffer for
processing.

---

## Architecture

```
Antenna (RX2 port)
    │  analog RF signal
    ▼
AD9361 RF chip  (inside B205mini)
    │  down-converts to baseband, ADC at sample rate
    ▼
FPGA  (inside B205mini)
    │  packs samples into USB transfer packets
    ▼
USB 3.0
    │
    ▼
uhd.dll  (on the PC)
    │  DMA into internal ring buffer
    ▼
rx_stream->recv()  →  buff[]   ← your processing goes here
```

All application logic runs on the PC. The B205mini is a USB peripheral — it
has no CPU to run code on.

---

## Files

| File | Purpose |
|---|---|
| `main.cpp` | Connects to USRP, configures RX, receive loop |
| `CMakeLists.txt` | Build config (hardcoded UHD paths for this machine) |
| `build.bat` | One-click Windows build script |
| `SETUP_NOTES.md` | Full setup history, problems hit, and solutions |
| `CLAUDE.md` | Project context for AI-assisted development |

---

## Hardware

- **USRP B205mini** connected via USB 3.0
- Antenna on the **RX2** port

---

## Dependencies (Windows)

| Dependency | Location |
|---|---|
| UHD 4.10.0.0 | `C:\Program Files\UHD` |
| Boost 1.86.0 headers | `C:\Users\2406h\boost\` |
| Boost 1.86.0 libs | Radioconda pkg cache |
| MSVC 19.44 | VS 2022 Build Tools at `C:\Program Files (x86)\...` |

---

## Build

Open a **fresh CMD window** (not PowerShell, not a reused CMD session) and run:

```cmd
cd C:\Users\2406h\Documents\usrp_hello
build.bat
```

Output: `build\Release\usrp_to_grc.exe`

If the build fails with "input line too long", you are reusing a CMD window
that already ran `build.bat`. Open a new one.

---

## Run

```cmd
build\Release\usrp_to_grc.exe
```

Press **Ctrl+C** to stop. This sends a clean stream-stop command to the USRP
FPGA before exiting.

---

## Current RX parameters (hardcoded in main.cpp)

| Parameter | Value | Meaning |
|---|---|---|
| `rate` | `50e6` | 50 MS/s sample rate |
| `freq` | `2450e6` | 2.45 GHz center frequency (2.4 GHz ISM band) |
| `gain` | `30` | 30 dB RX gain |
| `bw` | `50e6` | 50 MHz analog filter bandwidth |

---

## Adding signal processing

IQ samples land in `buff[]` inside the receive loop in `main.cpp`:

```cpp
while (!stop_signal_called) {
    size_t num_rx_samps = rx_stream->recv(&buff.front(), buff.size(), md, 3.0);

    if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) continue;

    // buff[0..num_rx_samps-1] contains fresh IQ samples — process here
}
```

Each element is `std::complex<float>` where `.real()` = I and `.imag()` = Q.
These are the raw constellation coordinates of the received signal.

Useful standard library operations on a sample `s`:

| Expression | Result |
|---|---|
| `std::norm(s)` | Power (I² + Q²) |
| `std::abs(s)` | Amplitude √(I² + Q²) |
| `std::arg(s)` | Phase angle atan2(Q, I) |

For heavy processing, use a second thread so the receive loop is never blocked.
See `SETUP_NOTES.md` for the producer-consumer pattern.
