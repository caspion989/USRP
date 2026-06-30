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
#include <cmath>

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

// ─── DSP worker — runs on its own thread ─────────────
// Pops batches from the queue and applies a frequency shift.
// sample_offset tracks the absolute sample index across batches
// so the complex exponential is continuous (no phase jump at batch edges).
void dsp_worker(SampleQueue& sq, double f_shift_hz, double sample_rate)
{
    uhd::set_thread_priority_safe();   // give DSP thread RT priority too

    const float two_pi_f_over_fs = static_cast<float>(2.0 * M_PI * f_shift_hz / sample_rate);
    size_t sample_offset = 0;

    while (true) {
        // ── Wait for a batch ──────────────────────────
        std::vector<std::complex<float>> batch;
        {
            std::unique_lock<std::mutex> lock(sq.mtx);
            // Spurious-wake-up safe: predicate is re-checked after every wake.
            sq.cv.wait(lock, [&] {
                return !sq.q.empty() || stop_signal_called.load();
            });

            if (sq.q.empty()) break;   // stopped and queue fully drained

            batch = std::move(sq.q.front());
            sq.q.pop();
        }
        // Notify the producer in case it was blocking on MAX_DEPTH.
        sq.cv.notify_one();

        // ── Frequency shift ───────────────────────────
        // Multiply each sample by e^(j * 2π * f_shift * n / fs).
        for (size_t i = 0; i < batch.size(); ++i) {
            float phase = two_pi_f_over_fs * static_cast<float>(sample_offset + i);
            batch[i] *= std::complex<float>(std::cos(phase), std::sin(phase));
        }
        sample_offset += batch.size();

        // ── Your further processing goes here ─────────
        // batch[] now contains frequency-shifted IQ samples.
        // e.g. write to file, run FFT, demodulate, etc.
        (void)batch;
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

    double rate(50e6);
    double freq(2450e6);
    double gain(30);
    double bw(50e6);

    double f_shift_hz = 100e3; // Positive = shift up, negative = shift down.

    std::printf("Creating the usrp device with: %s...\n", device_args.c_str());
    uhd::usrp::multi_usrp::sptr usrp = uhd::usrp::multi_usrp::make(device_args);

    usrp->set_clock_source(ref);
    usrp->set_rx_subdev_spec(subdev);
    std::cout << "Using Device: " << usrp->get_pp_string() << std::endl;

    usrp->set_rx_rate(rate);
    std::printf("Actual RX Rate: %f Msps\n", usrp->get_rx_rate() / 1e6);

    uhd::tune_request_t tune_request(freq);
    usrp->set_rx_freq(tune_request);
    std::printf("Actual RX Freq: %f MHz\n", usrp->get_rx_freq() / 1e6);

    usrp->set_rx_gain(gain);
    std::printf("Actual RX Gain: %f dB\n", usrp->get_rx_gain());

    usrp->set_rx_bandwidth(bw);
    std::printf("Actual RX Bandwidth: %f MHz\n", usrp->get_rx_bandwidth() / 1e6);

    usrp->set_rx_antenna(ant);
    std::printf("Actual RX Antenna: %s\n\n", usrp->get_rx_antenna().c_str());

    uhd::stream_args_t stream_args("fc32");
    uhd::rx_streamer::sptr rx_stream = usrp->get_rx_stream(stream_args);

    uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
    stream_cmd.stream_now = true;
    rx_stream->issue_stream_cmd(stream_cmd);

    const size_t samples_per_packet = 1024;
    std::vector<std::complex<float>> recv_buf(samples_per_packet);
    uhd::rx_metadata_t md;

    // ── Start the DSP thread ──────────────────────────
    SampleQueue sq;
    std::thread dsp_thread(dsp_worker, std::ref(sq), f_shift_hz, rate);

    std::printf("Receiving -- press Ctrl+C to stop.\n\n");

    // ── Producer loop (this thread) ───────────────────
    while (!stop_signal_called) {
        size_t num_rx_samps =
            rx_stream->recv(&recv_buf.front(), recv_buf.size(), md, 3.0);

        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
            std::cerr << "Receiver error: " << md.strerror() << std::endl;
            continue;
        }

        // Copy only the valid samples into a fresh vector and hand it off.
        std::vector<std::complex<float>> batch(recv_buf.begin(), recv_buf.begin() + num_rx_samps);

        {
            std::unique_lock<std::mutex> lock(sq.mtx);
            // Back-pressure: block if the DSP thread is too slow.
            sq.cv.wait(lock, [&] {
                return sq.q.size() < SampleQueue::MAX_DEPTH || stop_signal_called.load();
            });
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
