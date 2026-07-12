#include <uhd/utils/thread_priority.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/exception.hpp>
#include <uhd/types/tune_request.hpp>
#include <uhd/types/stream_cmd.hpp>
#include <uhd/stream.hpp>
#include <iostream>
#include <complex>
#include <csignal>
#include <atomic>
#include <vector>
#include <cstdio>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <fstream>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <algorithm>

static std::atomic<bool> stop_signal_called(false);
void sig_int_handler(int) { stop_signal_called = true; }

// ─── Shared queue between the recv thread (producer)
//     and the DSP thread (consumer) ─────────────────
struct SampleQueue {
    std::queue<std::vector<std::complex<float>>> q;
    std::mutex mtx;
    std::condition_variable cv;
    // Back-pressure limit: the producer blocks rather than growing the queue
    // without bound once this many batches are pending. Sized to absorb the
    // periodic FFT burst: while the DSP thread computes one 524288-point FFT
    // (tens of ms, not draining the queue), the producer keeps pushing at
    // ~1221 batches/sec. 256 batches = 256*4096/5e6 ≈ 210 ms of buffering,
    // comfortably above the ~105 ms window-fill/FFT ceiling → no overflow.
    static constexpr size_t MAX_DEPTH = 256;
};


// ─── In-place iterative radix-2 Cooley-Tukey FFT ─────
// Transforms `a` from time domain to frequency domain, in place.
// a.size() MUST be a power of 2 (caller guarantees this).
void fft_inplace(std::vector<std::complex<float>>& a)
{
    const size_t n = a.size();
    if (n <= 1) return;

    // Step 1: bit-reversal permutation — reorder samples so the butterflies
    // below can run in place. (Standard iterative-FFT reordering.)
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }

    // Step 2: butterfly stages, doubling the transform length each pass.
    // acos(-1.0) == pi — avoids the MSVC M_PI-not-defined gotcha.
    const double two_pi = 2.0 * std::acos(-1.0);
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = -two_pi / static_cast<double>(len);   // negative angle = forward FFT
        std::complex<float> wlen(static_cast<float>(std::cos(ang)),
                                 static_cast<float>(std::sin(ang)));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k) {
                std::complex<float> u = a[i + k];
                std::complex<float> v = a[i + k + len / 2] * w;
                a[i + k]           = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;   // advance the twiddle factor
            }
        }
    }
}

void fft(SampleQueue& sq, size_t samples_per_packet, double rate, double center_freq)
{
    // Accumulate this many REAL samples per FFT evaluation. No zero-padding:
    // the whole window is real data, so this gives TRUE resolution rate/N.
    // 2^19 = 524288 samples ≈ 0.105 s at 5 MSPS → ~9.5 evals/sec, ~9.5 Hz res.
    // MUST be a power of 2 (radix-2 FFT) AND a whole multiple of
    // samples_per_packet so the window fills to exactly FFT_SIZE.
    // (524288 = 128 × 4096.)
    const size_t FFT_SIZE = 524288;

    std::vector<std::complex<float>> window;
    window.reserve(FFT_SIZE);   // allocate capacity ONCE; cleared & refilled per cycle

    const double resolution = rate / static_cast<double>(FFT_SIZE);   // Hz per bin (true)
    std::printf("FFT: accumulating %zu real samples per evaluation\n"
                "  Eval rate:       %.2f /sec\n"
                "  True resolution: %.2f Hz\n\n",
                FFT_SIZE, resolution, resolution);   // eval rate == resolution == rate/N

    while (true) {
        std::vector<std::complex<float>> batch;
        {
            std::unique_lock<std::mutex> lock(sq.mtx);
            sq.cv.wait(lock, [&] {return !sq.q.empty() || stop_signal_called.load();}); //make sure sq is not empty before popping

            if (sq.q.empty()) break;   // stopped and queue fully drained - active in case of shutdown

            batch = std::move(sq.q.front());
            sq.q.pop(); // remove the batch from the queue
        }
        sq.cv.notify_one();   // unblock producer if it was waiting on MAX_DEPTH

        // Append this batch to the accumulation window. On a short recv() read
        // (batch smaller than samples_per_packet), zero-fill the missing tail so
        // the window still advances in clean samples_per_packet steps and lands
        // on exactly FFT_SIZE — instead of dropping the incomplete batch.
        window.insert(window.end(), batch.begin(), batch.end()); 
        if (batch.size() < samples_per_packet)
            window.insert(window.end(), samples_per_packet - batch.size(), std::complex<float>(0.0f, 0.0f));

        if (window.size() < FFT_SIZE)
            continue;   // window not full yet — keep collecting batches

        fft_inplace(window);   // window now holds FFT_SIZE frequency-domain bins

        // Find the strongest bin, skipping DC (bin 0). Direct-conversion
        // receivers like the B205mini put an LO-leakage / DC-offset spike at
        // bin 0 (the tuned center frequency), which would otherwise always win.
        const size_t n = window.size();
        size_t peak_bin  = 1;
        float  peak_mag2 = std::norm(window[1]);   // norm() = |z|^2, no sqrt
        for (size_t k = 2; k < n; ++k) {
            float m2 = std::norm(window[k]);
            if (m2 > peak_mag2) {
                peak_mag2 = m2;
                peak_bin  = k;
            }
        }

        // Convert bin index → signed baseband offset in Hz.
        // Bins [0 .. n/2) are positive offsets; bins [n/2 .. n) are negative.
        double bin_index = (peak_bin < n / 2)
                               ? static_cast<double>(peak_bin)
                               : static_cast<double>(peak_bin) - static_cast<double>(n);
        double freq_offset = bin_index * rate / static_cast<double>(n);
        double signal_freq = center_freq + freq_offset;

        double magnitude = std::sqrt(peak_mag2) / static_cast<double>(n);
        std::printf("Peak: %.5f MHz  (offset %+.3f kHz, bin %zu, mag %.4f)\n",
                    signal_freq / 1e6, freq_offset / 1e3, peak_bin, magnitude);

        window.clear();   // start the next (non-overlapping) window; keeps capacity
    }
}

int UHD_SAFE_MAIN(int argc, char* argv[])
{
    uhd::set_thread_priority_safe();
    std::signal(SIGINT, &sig_int_handler);

    std::string device_args("type=b200");
    std::string subdev("A:A");
    std::string ant("RX2");
    std::string ref("internal");

    double rate(5e6);
    double freq(1000e6);
    double gain(30);
    double bw(5e6);

    std::printf("Creating the usrp device with: %s...\n", device_args.c_str());
    uhd::usrp::multi_usrp::sptr usrp = uhd::usrp::multi_usrp::make(device_args); // create the USRP device instance

    usrp->set_clock_source(ref); // set the clock source to internal or external
    usrp->set_rx_subdev_spec(subdev); // map the subdevice to the RX channel - there are divices with multiple RX channels, so we need to specify which one to use
    std::cout << "Using Device: " << usrp->get_pp_string() << std::endl; // print the device information

    usrp->set_rx_rate(rate); // set the RX sample rate
    std::printf("Actual RX Rate: %f Msps\n", usrp->get_rx_rate() / 1e6);

    uhd::tune_request_t tune_request(freq); // create a tune request for the desired frequency
    usrp->set_rx_freq(tune_request); // set the RX frequency using the tune request
    std::printf("Actual RX Freq: %f MHz\n", usrp->get_rx_freq() / 1e6);

    usrp->set_rx_gain(gain); // set the RX gain
    std::printf("Actual RX Gain: %f dB\n", usrp->get_rx_gain());

    usrp->set_rx_bandwidth(bw); // set the RX bandwidth
    std::printf("Actual RX Bandwidth: %f MHz\n", usrp->get_rx_bandwidth() / 1e6);

    usrp->set_rx_antenna(ant); // set the RX antenna
    std::printf("Actual RX Antenna: %s\n\n", usrp->get_rx_antenna().c_str());

    uhd::stream_args_t stream_args("fc32"); // set the stream format to complex float32
    uhd::rx_streamer::sptr rx_stream = usrp->get_rx_stream(stream_args); // create a RX streamer for the specified stream format

    uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS); // create a stream command to start continuous streaming
    stream_cmd.stream_now = true; 
    rx_stream->issue_stream_cmd(stream_cmd); // issue the stream command to start streaming

    const size_t samples_per_packet = 4096;
    std::vector<std::complex<float>> recv_buf(samples_per_packet);
    uhd::rx_metadata_t md; // metadata object to hold information about the received samples

    // ── Start the DSP thread ──────────────────────────
    SampleQueue sq;
    std::thread dsp_thread(fft, std::ref(sq), samples_per_packet, rate, freq);

    std::printf("Receiving -- press Ctrl+C to stop.\n\n");

    // ── Producer loop (this thread) ───────────────────
    while (!stop_signal_called) {
        size_t num_rx_samps = rx_stream->recv(&recv_buf.front(), recv_buf.size(), md, 3.0);

        // Check for errors in the received samples
        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
            std::cerr << "Receiver error: " << md.strerror() << std::endl;
            continue;
        }

        // Copy only the valid samples into a fresh vector and hand it off.
        std::vector<std::complex<float>> batch(recv_buf.begin(), recv_buf.begin() + num_rx_samps);

        {
            std::unique_lock<std::mutex> lock(sq.mtx);
            // Back-pressure: block if the DSP thread is too slow.
            sq.cv.wait(lock, [&] {return sq.q.size() < SampleQueue::MAX_DEPTH || stop_signal_called.load();});
            sq.q.push(std::move(batch)); 
        }
        sq.cv.notify_one();   // wake the DSP thread
    }

    // ── Clean shutdown ────────────────────────────────
    stream_cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
    rx_stream->issue_stream_cmd(stream_cmd);

    // Wake the DSP thread so it can drain the queue and exit.
    sq.cv.notify_all();
    dsp_thread.join();

    std::cout << "Done." << std::endl;
    return EXIT_SUCCESS;
}
