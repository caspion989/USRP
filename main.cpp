// usrp_to_grc: connects to a USRP B205mini via UHD, configures it, then
// streams received IQ samples to GNU Radio Companion over UDP so they can
// be visualized live (e.g. with a UDP Source -> QT GUI Sink flowgraph).

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

// Winsock2 for UDP (no Boost needed)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

static std::atomic<bool> stop_signal_called(false);
void sig_int_handler(int) { stop_signal_called = true; }

int UHD_SAFE_MAIN(int argc, char* argv[])
{
    uhd::set_thread_priority_safe(); // Set the thread priority to real-time if possible
    std::signal(SIGINT, &sig_int_handler); // Register Ctrl+C signal handler SIGINT is not defined in Windows, but this is for cross-platform compatibility

    // ------------------------------------------------------------------
    // USRP configuration (B205mini-specific values)
    // ------------------------------------------------------------------
    std::string device_args("type=b200");
    std::string subdev("A:A");
    std::string ant("RX2");
    std::string ref("internal");

    double rate(50e6);
    double freq(2450e6);
    double gain(30);
    double bw(50e6);

    // ------------------------------------------------------------------
    // Where GNU Radio Companion's UDP Source block will be listening
    // ------------------------------------------------------------------
    const char* grc_host = "127.0.0.1";
    const unsigned short grc_port = 12345;

    // ------------------------------------------------------------------
    // Create and configure the USRP
    // ------------------------------------------------------------------
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

    // ------------------------------------------------------------------
    // Set up the RX streamer
    // ------------------------------------------------------------------
    uhd::stream_args_t stream_args("fc32");
    uhd::rx_streamer::sptr rx_stream = usrp->get_rx_stream(stream_args);

    uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
    stream_cmd.stream_now = true;
    rx_stream->issue_stream_cmd(stream_cmd);

    // ------------------------------------------------------------------
    // Set up Winsock2 UDP socket
    // ------------------------------------------------------------------
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(grc_port);
    inet_pton(AF_INET, grc_host, &dest.sin_addr);

    // 1024 complex<float> samples = 8192 bytes per UDP packet
    const size_t samples_per_packet = 1024;
    std::vector<std::complex<float>> buff(samples_per_packet);
    uhd::rx_metadata_t md;

    std::printf("Streaming to GNU Radio Companion at %s:%u -- press Ctrl+C to stop.\n\n",
                grc_host, grc_port);

    while (!stop_signal_called) {
        size_t num_rx_samps = rx_stream->recv(&buff.front(), buff.size(), md, 3.0);

        if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
            std::cerr << "Receiver error: " << md.strerror() << std::endl;
            continue;
        }

        sendto(sock,
               reinterpret_cast<const char*>(buff.data()),
               static_cast<int>(num_rx_samps * sizeof(std::complex<float>)),
               0,
               reinterpret_cast<sockaddr*>(&dest),
               sizeof(dest));
    }

    // ------------------------------------------------------------------
    // Clean shutdown
    // ------------------------------------------------------------------
    stream_cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
    rx_stream->issue_stream_cmd(stream_cmd);
    closesocket(sock);
    WSACleanup();

    std::cout << "Done." << std::endl;
    return EXIT_SUCCESS;
}
