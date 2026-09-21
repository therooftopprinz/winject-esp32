#include "radio_sim.hpp"

#include <arpa/inet.h>
#include <getopt.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace
{

volatile sig_atomic_t g_run = 1;

void on_signal(int)
{
    g_run = 0;
}

bool parse_endpoint(const char* text, sockaddr_in* out)
{
    if (text == nullptr || out == nullptr)
    {
        return false;
    }
    std::string s(text);
    const auto colon = s.rfind(':');
    if (colon == std::string::npos)
    {
        return false;
    }
    const std::string host = s.substr(0, colon);
    const std::string port_s = s.substr(colon + 1);
    char* end = nullptr;
    const long port = strtol(port_s.c_str(), &end, 10);
    if (end == port_s.c_str() || *end != '\0' || port < 1 || port > 65535)
    {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(static_cast<uint16_t>(port));
    if (host.empty() || host == "*" || host == "0.0.0.0")
    {
        out->sin_addr.s_addr = htonl(INADDR_ANY);
        return true;
    }
    if (inet_pton(AF_INET, host.c_str(), &out->sin_addr) != 1)
    {
        return false;
    }
    return true;
}

void usage(const char* argv0)
{
    fprintf(stderr,
            "Usage: %s --console HOST:PORT --air-bind HOST:PORT "
            "[--air-peer HOST:PORT]...\n"
            "\n"
            "Behavioral winject-esp32 radio sim (UDP console + MPDU "
            "inject/forward + air).\n"
            "Default console port should not be 22 on a host (SSH). Example:\n"
            "\n"
            "  %s --console 127.0.0.1:10022 --air-bind 127.0.0.1:19001 \\\n"
            "     --air-peer 127.0.0.1:19002\n"
            "\n"
            "  %s --console 127.0.0.1:10023 --air-bind 127.0.0.1:19002 \\\n"
            "     --air-peer 127.0.0.1:19001\n"
            "\n"
            "Options:\n"
            "  --console HOST:PORT   UDP console bind (manager device:port)\n"
            "  --air-bind HOST:PORT  UDP air RX bind\n"
            "  --air-peer HOST:PORT  Air TX peer (repeatable)\n"
            "  --ip A.B.C.D          IP shown in status (default: console "
            "host or 127.0.0.1)\n"
            "  -h, --help            This help\n",
            argv0, argv0, argv0);
}

}  // namespace

int main(int argc, char** argv)
{
    sockaddr_in console{};
    sockaddr_in air_bind{};
    std::vector<sockaddr_in> peers;
    uint32_t display_ip = 0;
    bool have_console = false;
    bool have_air = false;

    static const option k_opts[] = {
        {"console", required_argument, nullptr, 'c'},
        {"air-bind", required_argument, nullptr, 'a'},
        {"air-peer", required_argument, nullptr, 'p'},
        {"ip", required_argument, nullptr, 'i'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    int opt = 0;
    while ((opt = getopt_long(argc, argv, "hc:a:p:i:", k_opts, nullptr)) !=
           -1)
    {
        switch (opt)
        {
            case 'c':
                if (!parse_endpoint(optarg, &console))
                {
                    fprintf(stderr, "bad --console %s\n", optarg);
                    return 2;
                }
                have_console = true;
                break;
            case 'a':
                if (!parse_endpoint(optarg, &air_bind))
                {
                    fprintf(stderr, "bad --air-bind %s\n", optarg);
                    return 2;
                }
                have_air = true;
                break;
            case 'p':
            {
                sockaddr_in peer{};
                if (!parse_endpoint(optarg, &peer) ||
                    peer.sin_addr.s_addr == 0)
                {
                    fprintf(stderr, "bad --air-peer %s\n", optarg);
                    return 2;
                }
                peers.push_back(peer);
                break;
            }
            case 'i':
            {
                in_addr a{};
                if (inet_pton(AF_INET, optarg, &a) != 1)
                {
                    fprintf(stderr, "bad --ip %s\n", optarg);
                    return 2;
                }
                display_ip = a.s_addr;
                break;
            }
            case 'h':
                usage(argv[0]);
                return 0;
            default:
                usage(argv[0]);
                return 2;
        }
    }

    if (!have_console || !have_air)
    {
        usage(argv[0]);
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    radio_sim sim;
    if (!sim.start(console, air_bind, peers, display_ip))
    {
        fprintf(stderr, "sim start failed (bind?): errno=%d\n", errno);
        return 1;
    }

    char cstr[64];
    char astr[64];
    inet_ntop(AF_INET, &console.sin_addr, cstr, sizeof(cstr));
    inet_ntop(AF_INET, &air_bind.sin_addr, astr, sizeof(astr));
    fprintf(stderr,
            "sim-winject-esp32 console=%s:%u air=%s:%u peers=%zu\n", cstr,
            ntohs(console.sin_port), astr, ntohs(air_bind.sin_port),
            peers.size());

    while (g_run)
    {
        pollfd fds[3];
        nfds_t nfd = 0;
        const int cfd = sim.console_fd();
        const int ifd = sim.inject_fd();
        const int afd = sim.air_fd();
        if (cfd >= 0)
        {
            fds[nfd].fd = cfd;
            fds[nfd].events = POLLIN;
            fds[nfd].revents = 0;
            ++nfd;
        }
        size_t inject_idx = static_cast<size_t>(-1);
        if (ifd >= 0)
        {
            inject_idx = nfd;
            fds[nfd].fd = ifd;
            fds[nfd].events = POLLIN;
            fds[nfd].revents = 0;
            ++nfd;
        }
        size_t air_idx = static_cast<size_t>(-1);
        if (afd >= 0)
        {
            air_idx = nfd;
            fds[nfd].fd = afd;
            fds[nfd].events = POLLIN;
            fds[nfd].revents = 0;
            ++nfd;
        }
        if (nfd == 0)
        {
            break;
        }
        const int pr = ::poll(fds, nfd, 200);
        if (pr < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("poll");
            break;
        }
        if (pr == 0)
        {
            continue;
        }
        // Console is always index 0 when present.
        if (cfd >= 0 && (fds[0].revents & (POLLIN | POLLERR | POLLHUP)))
        {
            sim.on_console_readable();
        }
        if (inject_idx != static_cast<size_t>(-1) &&
            (fds[inject_idx].revents & (POLLIN | POLLERR | POLLHUP)))
        {
            sim.on_inject_readable();
        }
        if (air_idx != static_cast<size_t>(-1) &&
            (fds[air_idx].revents & (POLLIN | POLLERR | POLLHUP)))
        {
            sim.on_air_readable();
        }
    }

    sim.stop();
    fprintf(stderr, "sim-winject-esp32 stopped\n");
    return 0;
}
