#include "endpoint/tcp_server_endpoint.h"

#include <errno.h>
#include <string.h>

#include "log.h"

tcp_server_endpoint::~tcp_server_endpoint()
{
    close();
}

bool tcp_server_endpoint::open(::reactor& reactor, const upstream_config_s& cfg)
{
    if (!tcp_endpoint::open(reactor, cfg))
    {
        return false;
    }
    listen_sock = make_tcp4();
    sockaddr_in addr = {};
    if (listen_sock.fd() < 0 || !parse_host_port(cfg.bind_address, &addr) ||
        listen_sock.bind(addr) < 0 || listen_sock.listen(1) < 0)
    {
        LOG_ERR("tcp listen %s: %s", cfg.bind_address.c_str(), strerror(errno));
        return false;
    }
    return reactor.add_read_rdy(listen_sock.fd(),
                                [this]()
                                {
                                    on_listen();
                                });
}

void tcp_server_endpoint::close()
{
    tcp_endpoint::close();
    if (listen_sock.fd() >= 0)
    {
        if (reactor != nullptr)
        {
            reactor->rem_read_rdy(listen_sock.fd());
        }
        close_socket(&listen_sock);
    }
}

void tcp_server_endpoint::on_peer_end()
{
    if (client_sock.fd() >= 0)
    {
        // Keep the camera gst socket; peer dropped, probe CONNECT again.
        stream.reset();
        stream.local_up();
        kick_tx();
        sync_rx_accept();
        return;
    }
    close_client();
    stream.reset();
}

void tcp_server_endpoint::on_listen()
{
    if (listen_sock.fd() < 0)
    {
        return;
    }
    sockaddr_in addr = {};
    bfc::socket accepted = listen_sock.accept(addr);
    if (accepted.fd() < 0)
    {
        return;
    }
    if (client_sock.fd() >= 0)
    {
        LOG_WRN("tcp replacing existing client");
        close_client();
        stream.reset();
    }
    if (!set_nonblock(accepted.fd()))
    {
        return;
    }
    apply_buffers(accepted);
    client_sock = std::move(accepted);
    stream.reset();
    stream.local_up();
    if (!watch_client_write() || !start_rx_thread())
    {
        close_client();
        stream.reset();
        return;
    }
    reactor->req_write(client_sock.fd());
    kick_tx();
    LOG_INF("tcp accepted %s", sockaddr_to_string(addr).c_str());
}
