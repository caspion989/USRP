# UHD Reference — Initializing the PC + SDR to Receive

This document explains every UHD function call in [main.cpp](main.cpp), in the
exact order they must happen, and *why* each step is required to get IQ samples
flowing from the antenna into `recv_buf[]`.

UHD (USRP Hardware Driver) is the library that talks to the B205mini over USB.
Your program is the "host"; the B205mini is a dumb USB peripheral with an FPGA
and an RF chip but no CPU — every decision is made on the PC and pushed down.

---

## The big picture — the order that matters

```
1. set thread priority        (host side — so the OS doesn't starve the recv loop)
2. install Ctrl+C handler      (host side — clean shutdown)
3. make() the device           (opens USB, loads FPGA image, creates the driver object)
4. configure the front-end     (clock, subdev, rate, freq, gain, bandwidth, antenna)
5. get_rx_stream()             (build the pipe that carries samples host←FPGA)
6. issue_stream_cmd(START)     (tell the FPGA to actually start sending)
7. recv() in a loop            (pull samples off the pipe into your buffer)
8. issue_stream_cmd(STOP)      (tell the FPGA to stop)
```

Steps 3→6 **must** be in this order. You cannot stream before configuring, and
you cannot configure before the device object exists.

---

## Universal API vs. hardware-specific values

It's easy to look at `main.cpp` and assume the whole thing — calls *and*
values — is the standard way to bring up any Ettus SDR. Only half of that is
true.

**Universal (true for every UHD-supported USRP):** the *method calls* and the
8-step order above. `set_clock_source()`, `set_rx_subdev_spec()`,
`set_rx_rate()`, `set_rx_freq()`, `set_rx_gain()`, `set_rx_bandwidth()`,
`set_rx_antenna()`, `get_rx_stream()`, `issue_stream_cmd()` exist identically
on a B205mini, a B210, an X310, or an N321. That's the entire point of the
`multi_usrp` API — one interface, any device.

**Not universal (specific to *this* B205mini):** the string/numeric values
you pass to those calls.

| Value in `main.cpp` | Why it's B205mini-specific |
|---|---|
| `device_args = "type=b200"` | USB devices identify via `type=`/`serial=`. Networked devices (X3xx, N3xx) use `addr=192.168.x.x` instead and don't need `type=` at all. |
| `subdev = "A:A"` | Depends on the daughterboard/motherboard. Multi-channel devices use different slot:channel strings, e.g. `"A:A A:B"` for two RX channels. |
| `ant = "RX2"` | Depends on the daughterboard. Not every board has an `RX2` port, and some have more antenna options (`CAL`, `TX/RX`, etc). |
| `ref = "internal"` | The clock-source *concept* is universal, but which options (`"external"`, `"gpsdo"`, `"mimo"`) are valid depends on the hardware. |
| `rate`/`freq`/`gain`/`bw` numeric values | Constrained by that specific device's tunable ranges — see the range-query methods below. |

**Rule of thumb**: same 8-step recipe everywhere, different fill-in-the-blank
values per device. Never assume a value from one USRP's example code applies
to another — always confirm with `uhd_usrp_probe` run against the actual
target hardware.

---

## Step 1 — `uhd::set_thread_priority_safe()`

```cpp
uhd::set_thread_priority_safe();
```

Asks the OS to raise the priority of the current thread (the receive thread).
"Safe" means: if the OS refuses (e.g. you lack permission), it prints a warning
and continues instead of crashing.

**Why**: `recv()` has a hard deadline — if the OS schedules a browser tab over
your recv loop and the USRP's internal buffer fills, samples are dropped
(overflow). Higher priority means the OS preempts you less. See
[THREADING_NOTES.md] for the deadline discussion.

---

## Step 2 — `std::signal(SIGINT, &sig_int_handler)`

```cpp
std::signal(SIGINT, &sig_int_handler);
```

This is standard C, not UHD. It registers a function to run when you press
Ctrl+C, which sets the atomic `stop_signal_called = true`. The receive loop
checks that flag each iteration and exits cleanly, so the program can issue a
proper STOP command to the FPGA instead of being killed mid-stream.

---

## Step 3 — `uhd::usrp::multi_usrp::make()`

```cpp
std::string device_args("type=b200");
uhd::usrp::multi_usrp::sptr usrp = uhd::usrp::multi_usrp::make(device_args);
```

**The single most important call.** This:
- Scans USB for a device matching `type=b200` (the B205mini reports as `b200`)
- Opens the USB connection
- Uploads the FPGA image to the device
- Returns an `sptr` (a `shared_ptr`) to a `multi_usrp` object — your handle for
  every subsequent command

`multi_usrp` is UHD's high-level API. "multi" because it can drive several USRPs
as one unit; here it drives exactly one. Everything after this is a method call
on `usrp->`.

`device_args` is a key=value string. Other examples: `serial=xxxxx` to pick a
specific device if several are plugged in.

---

## Step 4 — configure the front-end

These calls program the AD9361 RF chip and the FPGA. Order among *these* is
flexible, but they must all happen after `make()` and before streaming.

### `set_clock_source("internal")`
```cpp
usrp->set_clock_source(ref);   // ref = "internal"
```
Selects the reference clock the device uses for timing. `"internal"` = use the
B205mini's own oscillator (correct for a standalone device). Other options
(`"external"`, `"gpsdo"`) are for lab setups with a shared reference.

### `set_rx_subdev_spec("A:A")`
```cpp
usrp->set_rx_subdev_spec(subdev);   // subdev = "A:A"
```
Chooses which physical front-end to use. The B205mini has a single RX/TX
front-end, addressed as `A:A` (slot A, channel A). This is hardware-specific —
do not change without checking `uhd_usrp_probe`.

> **Ordering caveat (from the Ettus tutorial):** although the step-4 calls are
> *mostly* order-independent, `set_rx_subdev_spec()` is the exception — the
> tutorial stresses that you should **select the subdevice first**, before any
> call that takes a channel number (`set_rx_rate`, `set_rx_freq`, etc.), because
> the subdev spec defines the channel mapping those later calls resolve against.
> The single-front-end B205mini rarely bites you here, but on multi-channel
> devices setting the subdev spec late silently applies your rate/freq/gain to
> the wrong channel.

### `get_pp_string()` — pretty-print device info
```cpp
std::cout << "Using Device: " << usrp->get_pp_string() << std::endl;
```
"pp" = **pretty print**. Returns a human-readable multi-line string describing
the connected device: motherboard name, serial number, FPGA/firmware versions,
and front-end details. Purely diagnostic — it configures nothing. Its job is to
let you *visually confirm* the driver bound to the right hardware before you
stream.

### `set_rx_rate(5e6)`
```cpp
usrp->set_rx_rate(rate);   // 5 MSPS
std::printf("Actual RX Rate: %f Msps\n", usrp->get_rx_rate() / 1e6);
```
Sets the **sample rate** — how many IQ samples per second the device produces.
5e6 = 5 million samples/sec. This determines your bandwidth (how wide a slice of
spectrum you capture) and your USB throughput.

**Note the `get_rx_rate()` read-back**: the hardware can only hit certain rates
(derived from its master clock by integer division). You *request* 5 MSPS; the
driver picks the closest achievable rate and you print what you *actually* got.
This request→readback pattern repeats for every setting below.

### `set_rx_freq(tune_request)`
```cpp
uhd::tune_request_t tune_request(freq);   // freq = 1000e6 = 1 GHz
usrp->set_rx_freq(tune_request);
std::printf("Actual RX Freq: %f MHz\n", usrp->get_rx_freq() / 1e6);
```
Sets the **center frequency** — which part of the RF spectrum to tune to.
1 GHz here. `tune_request_t` wraps the target frequency; UHD internally splits
this into an RF hardware tune (the analog mixer) plus an optional digital
offset. You can just pass the frequency and let UHD figure out the split.

### `set_rx_gain(30)`
```cpp
usrp->set_rx_gain(gain);   // 30 dB
```
Sets the **receive amplifier gain** in dB. Higher = weak signals amplified more,
but too high clips strong signals and adds noise. 30 dB is a mid-range starting
point.

### `set_rx_bandwidth(5e6)`
```cpp
usrp->set_rx_bandwidth(bw);   // 5 MHz
```
Sets the **analog anti-aliasing filter width**. Usually matched to the sample
rate. This filters out signals outside your band of interest *before* they get
digitized, preventing aliasing.

### `set_rx_antenna("RX2")`
```cpp
usrp->set_rx_antenna(ant);   // "RX2"
```
Selects which physical antenna port to receive from. The B205mini has `RX2`
(receive) and `TX/RX` (shared). Your antenna cable must be physically plugged
into the port named here, or you receive nothing.

---

## Step 5 — build the receive stream

```cpp
uhd::stream_args_t stream_args("fc32");
uhd::rx_streamer::sptr rx_stream = usrp->get_rx_stream(stream_args);
```

`get_rx_stream()` creates the **pipe** that actually carries samples from the
FPGA up to the host. Configuration (step 4) set *how* the radio behaves;
this creates the *channel* the data flows through.

`stream_args_t("fc32")` sets the sample format the host receives:
- **`fc32`** = "float complex 32" → each sample is a `std::complex<float>`
  (two 32-bit floats: I and Q). This is why `recv_buf` is
  `std::vector<std::complex<float>>`.
- Alternative `sc16` = 16-bit ints (half the USB bandwidth, less convenient to
  process). `fc32` trades bandwidth for ease of use.

`rx_streamer::sptr` is your handle for the two streaming operations: `recv()`
and `issue_stream_cmd()`.

### All available `cpu_format` / `otw_format` values

`stream_args_t` takes two independent format strings — they don't have to
match, and UHD converts between them for you. Source: `uhd/stream.hpp` doc
comments in the local UHD 4.10.0.0 install.

**`cpu_format`** — the type UHD hands you in host RAM:

| String | Type | Notes |
|---|---|---|
| `fc64` | `std::complex<double>` | highest precision, 16 bytes/sample, rarely needed |
| `fc32` | `std::complex<float>` | what `main.cpp` uses — 8 bytes/sample, matches `recv_buf`'s `std::complex<float>` |
| `sc16` | `std::complex<int16_t>` | raw fixed-point, 4 bytes/sample, no float conversion overhead |
| `sc8` | `std::complex<int8_t>` | 2 bytes/sample, lowest precision |

Listed in the header but **not implemented** (shown only for naming
convention): `f32`, `f64`, `s16`, `s8` — real-only (non-complex) variants.

**`otw_format`** — what actually goes over USB/Ethernet (the second
constructor arg; `main.cpp` leaves it blank, so UHD uses the device default):

| String | Notes |
|---|---|
| `sc16` | 16-bit I/Q, default on most devices including the B205mini |
| `sc8` | 8-bit I/Q — halves USB bandwidth, lets you push higher sample rates, at the cost of dynamic range/quantization noise |
| `sc12` | only supported on some devices |

The header explicitly warns: "not all combinations of CPU and OTW format have
conversion support" — verify a pairing actually builds/runs before relying on
it. If you ever hit USB bandwidth limits at high `rate`, try
`uhd::stream_args_t stream_args("fc32", "sc8")` to trade dynamic range for
headroom.

---

## Step 6 — tell the FPGA to start sending

```cpp
uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
stream_cmd.stream_now = true;
rx_stream->issue_stream_cmd(stream_cmd);
```

Creating the stream (step 5) does **not** start data flowing — you must
explicitly command it.

- **`STREAM_MODE_START_CONTINUOUS`** — stream samples forever until told to
  stop (as opposed to "send exactly N samples then stop").
- **`stream_now = true`** — start immediately, don't wait for a scheduled
  timestamp. (Timed starts are used to synchronize multiple devices.)
- **`issue_stream_cmd()`** — sends the command down to the FPGA, which begins
  pushing samples into its internal buffer and up the USB pipe.

From this instant, the hardware buffer is filling. Your `recv()` loop must now
keep up.

---

## Step 7 — the receive loop

```cpp
const size_t samples_per_packet = 1024;
std::vector<std::complex<float>> recv_buf(samples_per_packet);
uhd::rx_metadata_t md;

while (!stop_signal_called) {
    size_t num_rx_samps =
        rx_stream->recv(&recv_buf.front(), recv_buf.size(), md, 3.0);

    if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
        std::cerr << "Receiver error: " << md.strerror() << std::endl;
        continue;
    }
    // ... hand samples off to the DSP/CSV thread ...
}
```

### `recv(buffer, max_samps, metadata, timeout)`
The workhorse. Pulls up to `max_samps` samples off the pipe into your buffer,
and returns how many it *actually* got (`num_rx_samps` — may be fewer than
requested).

| Argument | Meaning |
|---|---|
| `&recv_buf.front()` | raw pointer to where samples get written (see [CPP_PRIMITIVES.md] §12 on the C-API boundary) |
| `recv_buf.size()` | max samples this call may write (buffer capacity) |
| `md` | output: metadata about this batch — error codes, timestamps |
| `3.0` | timeout in seconds — give up if no samples arrive in 3s |

### `md` — the metadata / error channel
`recv()` reports problems through `md.error_code`, not through exceptions or
return values. Each call you must check it:
- **`ERROR_CODE_NONE`** — all good, `num_rx_samps` samples are valid.
- **`ERROR_CODE_OVERFLOW`** — you didn't call `recv()` fast enough, the hardware
  buffer overflowed and samples were dropped. (The RT-correct response is to log
  and keep going, never to stall — see [THREADING_NOTES.md].)
- **`ERROR_CODE_TIMEOUT`** — nothing arrived within the timeout.

`md.strerror()` turns the code into a readable string for the log.

---

## Step 8 — clean shutdown

```cpp
stream_cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
rx_stream->issue_stream_cmd(stream_cmd);
```

When Ctrl+C sets `stop_signal_called`, the loop exits and this reuses the same
`stream_cmd` object with **`STREAM_MODE_STOP_CONTINUOUS`** — telling the FPGA to
stop pushing samples. Without this the device would keep streaming into a buffer
nobody is draining. This is the counterpart to step 6.

(The `notify_all()` + `join()` after it are thread cleanup, not UHD — they drain
the sample queue and wait for the CSV thread to finish. See [CPP_PRIMITIVES.md].)

---

## Minimum requirements checklist — what's strictly needed vs. what main.cpp omits

`main.cpp` is a working minimal program, not a complete one. Here's the line
between "required to get valid samples" and "present in production-grade UHD
code but missing here."

### Strictly required to get any valid samples flowing

- Create the device — `multi_usrp::make()`
- Set a sample rate — `set_rx_rate()`
- Set a center frequency — `set_rx_freq()`
- Set a gain — `set_rx_gain()`
- Set an antenna — `set_rx_antenna()` (if the board has more than one option —
  some single-port boards default correctly without it)
- Get a streamer — `get_rx_stream()`
- Start streaming — `issue_stream_cmd(START)`
- Pull samples — `recv()` in a loop

Everything else has a working default, but skipping it means trusting
whatever the device booted with.

### Present in `main.cpp` but technically optional

- `set_clock_source()` — has a sane default (internal), explicit is safer
- `set_rx_subdev_spec()` — only changes behavior on multi-frontend devices;
  harmless but unnecessary on a single-frontend B205mini
- `set_rx_bandwidth()` — many boards pick a bandwidth matched to the sample
  rate automatically

### Missing from `main.cpp` that real-world UHD programs typically add

These are documented elsewhere in this file (see the linked section) but not
implemented in your code yet:

| Missing piece | Why it matters | Where it's documented here |
|---|---|---|
| **LO lock check** (`get_rx_sensor("lo_locked")`) | After `set_rx_freq()`, the local oscillator needs time to lock. If you start `recv()` before it locks, early samples can be garbage/noise. | "Sensors — check hardware health / lock status" section below |
| **`get_max_num_samps()`** for buffer sizing | `main.cpp` hardcodes `1024`; querying it is portable across transports/devices | "`get_max_num_samps()`" section below |
| **Distinguishing `TIMEOUT` vs `OVERFLOW`** | `main.cpp` treats all errors the same; the correct RT behavior differs per error (break vs. log-and-continue) | "Handling `TIMEOUT` and `OVERFLOW` distinctly" section below |
| **Range queries before setting** (`get_rx_gain_range()`, `get_rx_freq_range()`, etc.) | Setting a value outside the valid range silently clamps rather than erroring — querying first avoids surprises | "Querying valid ranges" section below |
| **Settling delay after tuning** | Some workflows sleep briefly (or poll `lo_locked`) after `set_rx_freq()` before trusting samples, especially on frequency hops | related to the LO lock check above |

None of these will stop the program from running — the B205mini will happily
stream *something* without them. They matter for **trusting the data you
capture**, not for getting the program to compile and run.

---

## Building & compiling (from the Ettus tutorial)

The API reference above is useless without a way to compile it. Ettus links UHD
programs against `libuhd`, and the canonical build path is **CMake**, not a bare
`g++` line — CMake is what finds the UHD headers/libraries and Boost for you.

### Required headers

The program needs these includes (the tutorial's minimal set):

```cpp
#include <uhd/utils/thread_priority.hpp>   // set_thread_priority_safe
#include <uhd/utils/safe_main.hpp>         // UHD_SAFE_MAIN
#include <uhd/usrp/multi_usrp.hpp>         // multi_usrp, the main handle
#include <uhd/exception.hpp>               // UHD exception types
#include <uhd/types/tune_request.hpp>      // tune_request_t
#include <boost/program_options.hpp>       // arg parsing (optional)
#include <boost/format.hpp>                // formatted console output
#include <boost/thread.hpp>                // threading helpers
#include <iostream>
```

### The CMake route (recommended by Ettus)

1. Copy `uhd/host/examples/init_usrp/CMakeLists.txt` — it's the official starter
   template that already locates UHD + Boost correctly.
2. Edit its `add_executable(...)` line to list your source file(s).
3. Put the edited `CMakeLists.txt` and your `.cpp` in an otherwise-empty folder.
4. Configure and build out-of-source:

```bash
mkdir build
cd build
cmake ../
make
```

5. Run the resulting binary:

```bash
./usrp_basic
```

A successful run prints each setting followed by the **actual** value the
hardware applied (the request→readback pattern), confirming the device bound and
configured correctly before any streaming begins.

### Where to find more example programs

- **In the source tree:** `uhd/host/examples/` on GitHub —
  <https://github.com/EttusResearch/uhd/>
- **After installing UHD:** the compiled examples live at
  `$prefix/lib/uhd/examples` (e.g. `rx_samples_to_file`, `benchmark_rate`),
  which are the best real-world references for streaming code like `main.cpp`.

---

## Concepts from the official Ettus tutorial that main.cpp simplifies

> Source: Ettus KB, *Getting Started with UHD and C++*
> (https://kb.ettus.com/Getting_Started_with_UHD_and_C%2B%2B).
> These are things the canonical tutorial emphasizes that `main.cpp` either
> hardcodes or handles more loosely. Worth knowing for correctness.

### `UHD_SAFE_MAIN` — why main is declared this way

```cpp
int UHD_SAFE_MAIN(int argc, char* argv[]) { ... }
```

`UHD_SAFE_MAIN` is a **macro** that expands into a normal `main()` wrapped in a
`try/catch`. UHD reports many failures (device not found, invalid args, USB
errors) by **throwing C++ exceptions**, not by return codes. If an exception
escaped `main()`, the program would `std::terminate()` with an ugly stack dump.
The macro catches it and prints a clean error + returns a non-zero exit code.

Roughly equivalent to:
```cpp
int main(int argc, char* argv[]) {
    try {
        return uhd_safe_main_impl(argc, argv);   // your actual code
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}
```

**Takeaway**: because UHD throws, any configuration call (`make`, `set_rx_freq`,
etc.) can throw. `UHD_SAFE_MAIN` is the safety net so you don't have to wrap every
one. If you ever move code *out* of `UHD_SAFE_MAIN`, add your own try/catch.

### `get_max_num_samps()` — the correct way to size the recv buffer

```cpp
size_t max_samps = rx_stream->get_max_num_samps();
std::vector<std::complex<float>> recv_buf(max_samps);
```

The tutorial sizes the buffer from the stream itself. `get_max_num_samps()`
returns the largest number of samples `recv()` can deliver in one call (the
transport's packet size). `main.cpp` hardcodes `1024` instead — which works, but
querying it is more portable across devices and transports. Passing a buffer
larger than this max just means `recv()` fills at most `max_samps` per call.

### Handling `TIMEOUT` and `OVERFLOW` distinctly

`main.cpp` treats every non-`NONE` error the same (print + continue). The
tutorial distinguishes them, which is the more correct RT behavior:

```cpp
if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
    std::cerr << "Timeout — no samples received\n";
    break;   // nothing is coming; give up rather than spin
}
if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
    std::cerr << "O" << std::flush;   // overflow — log briefly and KEEP GOING
    continue;                          // never stall the recv loop (see THREADING_NOTES.md)
}
if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
    throw std::runtime_error("Receiver error: " + md.strerror());  // real fault
}
```

- **TIMEOUT** → usually fatal (stream never started, or device unplugged) → break.
- **OVERFLOW** → expected under load; the classic UHD convention is to print a
  single `"O"` and continue. **Never** break or block on overflow.
- **Anything else** → a genuine fault worth throwing on.

### Finite acquisition — "receive exactly N samples then stop"

`main.cpp` uses `STREAM_MODE_START_CONTINUOUS` (stream forever). The tutorial also
shows the finite mode, useful for grabbing a fixed capture:

```cpp
uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE);
stream_cmd.num_samps  = total_samples_wanted;   // e.g. 1e6
stream_cmd.stream_now = true;
rx_stream->issue_stream_cmd(stream_cmd);
// recv() until you've collected num_samps — the device stops on its own,
// no STOP command needed.
```

| Mode | Behavior |
|---|---|
| `STREAM_MODE_START_CONTINUOUS` | Stream until you send STOP (what main.cpp uses) |
| `STREAM_MODE_STOP_CONTINUOUS` | The STOP command itself |
| `STREAM_MODE_NUM_SAMPS_AND_DONE` | Send exactly `num_samps`, then auto-stop |
| `STREAM_MODE_NUM_SAMPS_AND_MORE` | Send `num_samps` as part of a longer burst |

### `md.time_spec` — the timestamp of a batch

Beyond error codes, `md` also carries `md.time_spec` — the device timestamp of the
first sample in the batch (a `uhd::time_spec_t`). `main.cpp` ignores it, but it's
how you'd align samples to absolute time or detect gaps between batches.

---

## Other useful USRP methods (not in main.cpp, but worth knowing)

These are methods you'll reach for as the project grows. All are called on the
`usrp->` handle like the ones above.

### Querying valid ranges — "what can this hardware actually do?"

Every `set_*` has a matching range query. Call these *before* setting a value to
avoid silently getting clamped to the nearest supported one.

```cpp
uhd::meta_range_t r = usrp->get_rx_freq_range();   // min/max tunable frequency
uhd::gain_range_t g = usrp->get_rx_gain_range();   // min/max/step gain in dB
uhd::meta_range_t s = usrp->get_rx_rates();        // list of valid sample rates

std::cout << "Gain range: " << g.start() << " to " << g.stop() << " dB\n";
```

| Method | Returns |
|---|---|
| `get_rx_freq_range()` | Tunable frequency span (Hz) |
| `get_rx_gain_range()` | Gain min / max / step (dB) |
| `get_rx_rates()` | Supported sample rates |
| `get_rx_bandwidth_range()` | Valid analog filter widths |
| `get_rx_antennas()` | List of valid antenna port names (e.g. `RX2`, `TX/RX`) |

### Sensors — check hardware health / lock status

```cpp
// Is the local oscillator locked? (must be true for clean reception)
uhd::sensor_value_t lo = usrp->get_rx_sensor("lo_locked");
bool locked = lo.to_bool();

std::vector<std::string> names = usrp->get_rx_sensor_names();  // list what's available
```

`lo_locked` is the important one — if the LO (local oscillator) hasn't locked
after a `set_rx_freq`, your samples are garbage. Production code polls this after
tuning and waits until it reads true.

### Master clock rate — the root of all rates

```cpp
usrp->set_master_clock_rate(16e6);           // set the FPGA master clock
double mcr = usrp->get_master_clock_rate();  // read it back
```

Every sample rate the device can hit is derived from this master clock by integer
division. On the B2xx series it's tunable, which lets you reach sample rates the
default clock can't divide to cleanly.

### Timing — timestamps and synchronized starts

```cpp
usrp->set_time_now(uhd::time_spec_t(0.0));   // reset the device clock to zero
uhd::time_spec_t now = usrp->get_time_now(); // read current device time
```

Used with `stream_cmd.stream_now = false` + `stream_cmd.time_spec = ...` to start
streaming at a precise future timestamp — essential when synchronizing multiple
USRPs or aligning RX with TX.

### Device identity

```cpp
std::string name = usrp->get_mboard_name();      // motherboard model, e.g. "B205mini"
size_t n_rx = usrp->get_rx_num_channels();       // how many RX channels exist
```

### Where these come from

The full list is UHD's `multi_usrp` class. Reference:
- Official API: https://files.ettus.com/manual/classuhd_1_1usrp_1_1multi__usrp.html
- Or run `uhd_usrp_probe` from the command line to dump everything the connected
  device reports (rates, ranges, sensors) without writing any code.

---

## Quick reference — every UHD symbol in main.cpp

| Symbol | What it is |
|---|---|
| `uhd::set_thread_priority_safe()` | Raise recv-thread OS priority |
| `uhd::usrp::multi_usrp::make(args)` | Open device, load FPGA, return handle |
| `uhd::usrp::multi_usrp::sptr` | shared_ptr to the device handle |
| `usrp->set_clock_source()` | Select reference clock (internal) |
| `usrp->set_rx_subdev_spec()` | Select which front-end (A:A) |
| `usrp->get_pp_string()` | Human-readable device description (diagnostic) |
| `usrp->set_rx_rate()` | Sample rate (Hz) |
| `usrp->set_rx_freq()` | Center frequency (Hz) |
| `usrp->set_rx_gain()` | RX amplifier gain (dB) |
| `usrp->set_rx_bandwidth()` | Analog filter width (Hz) |
| `usrp->set_rx_antenna()` | Physical antenna port (RX2) |
| `usrp->get_rx_*()` | Read back the *actual* value the hardware set |
| `uhd::tune_request_t` | Wraps a target frequency for set_rx_freq |
| `uhd::stream_args_t("fc32")` | Sample format = complex<float> |
| `usrp->get_rx_stream()` | Create the host←FPGA sample pipe |
| `uhd::rx_streamer::sptr` | Handle for recv() and stream commands |
| `uhd::stream_cmd_t` | START / STOP command to the FPGA |
| `rx_stream->issue_stream_cmd()` | Send START/STOP to the device |
| `rx_stream->recv()` | Pull samples into your buffer |
| `uhd::rx_metadata_t` | Per-batch error codes / timestamps |
| `UHD_SAFE_MAIN` | Macro wrapping main() with UHD exception handling |

---

## The mental model

```
        CONFIGURE (step 4)              STREAM (steps 5-7)
        ─────────────────              ──────────────────
        "how the radio behaves"        "the data actually moving"

Antenna → RF chip → FPGA  ══════════ USB ══════════►  recv() → recv_buf[]
   ▲         ▲        ▲                                   ▲
  RX2      gain    sample rate                       your loop must
         freq/bw   START command                     keep draining this
```

Configuration programs the analog + FPGA behavior. The stream command opens the
floodgate. `recv()` is you bailing water fast enough to keep up. The whole
architecture exists because the B205mini has no CPU — the PC does all the work.