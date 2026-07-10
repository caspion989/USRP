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
    // Back-pressure limit: if the DSP thread falls behind this many
    // batches the producer blocks rather than growing without bound.
    static constexpr size_t MAX_DEPTH = 32;
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
    auto last_print = std::chrono::steady_clock::now();

    // Zero-padding: run the FFT on more points than we have real samples.
    // This does NOT add true resolution (that is fixed by the real sample
    // count / observation time) — it interpolates the spectrum, giving finer
    // bin spacing so a single peak can be located more precisely.
    const size_t ZERO_PAD_FACTOR = 4;                       // 4x -> 16384-point FFT
    const size_t fft_size = samples_per_packet * ZERO_PAD_FACTOR;
    std::vector<std::complex<float>> padded(fft_size);      // allocated ONCE, reused

    const double true_res    = rate / static_cast<double>(samples_per_packet);
    const double bin_spacing = rate / static_cast<double>(fft_size);
    std::printf("FFT: %zu real samples zero-padded to %zu points\n"
                "  New bin spacing (interpolated): %.1f Hz\n"
                "  True resolution (unchanged):    %.1f Hz\n\n",
                samples_per_packet, fft_size, bin_spacing, true_res);

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

        // A short recv() read can hand us a batch smaller than samples_per_packet.
        // Skip those so the zero-padded copy below always has the same real length.
        if (batch.size() != samples_per_packet)
            continue;

        // Zero-pad: copy the real samples into the front, leave the tail as zeros.
        // (Re-zero every iteration because fft_inplace overwrote the buffer last time.)
        std::fill(padded.begin(), padded.end(), std::complex<float>(0.0f, 0.0f));
        std::copy(batch.begin(), batch.end(), padded.begin());

        fft_inplace(padded);   // padded now holds frequency-domain bins

        // Find the strongest bin, skipping DC (bin 0). Direct-conversion
        // receivers like the B205mini put an LO-leakage / DC-offset spike at
        // bin 0 (the tuned center frequency), which would otherwise always win.
        const size_t n = padded.size();
        size_t peak_bin  = 1;
        float  peak_mag2 = std::norm(padded[1]);   // norm() = |z|^2, no sqrt
        for (size_t k = 2; k < n; ++k) {
            float m2 = std::norm(padded[k]);
            if (m2 > peak_mag2) {
                peak_mag2 = m2;
                peak_bin  = k;
            }
        }

        // Convert bin index → signed baseband offset in Hz. Zero-padding does not
        // change the sample rate, so bin spacing is rate / fft_size.
        // Bins [0 .. n/2) are positive offsets; bins [n/2 .. n) are negative.
        double bin_index = (peak_bin < n / 2)
                               ? static_cast<double>(peak_bin)
                               : static_cast<double>(peak_bin) - static_cast<double>(n);
        double freq_offset = bin_index * rate / static_cast<double>(n);
        double signal_freq = center_freq + freq_offset;

        // Throttle console output to ~1 Hz. At 5 MSPS / 4096 the FFT runs
        // ~1200x/second; printing every result would bottleneck this thread
        // and stall the producer (same trap the CSV writer had).
        auto now = std::chrono::steady_clock::now();
        if (now - last_print >= std::chrono::seconds(1)) {
            last_print = now;
            // Normalize by the REAL sample count — zero-padding adds no energy.
            double magnitude = std::sqrt(peak_mag2) / static_cast<double>(samples_per_packet);
            std::printf("Peak: %.4f MHz  (offset %+.1f kHz, bin %zu, mag %.4f)\n",
                        signal_freq / 1e6, freq_offset / 1e3, peak_bin, magnitude);
        }
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
