#include "endpoint/tcp_client_endpoint.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>

#include "log.h"

tcp_client_endpoint::~tcp_client_endpoint()
{
    close();
}

void tcp_client_endpoint::on_peer_end()
{
    // Close gst so tcpserversrc sees EOF.
    close_client();
    stream.reset();
}

void tcp_client_endpoint::maybe_connect()
{
    if (stream.wants_connect())
    {
        start_connect();
    }
}

void tcp_client_endpoint::on_incoming_connect()
{
    // TCP_CLIENT opens the local app socket when the peer CONNECT arrives.
    // Retransmitted CONNECT must never tear down a connecting/live local TCP
    // session — that EOS's tcpserversrc, which sends CLOSE back and makes the
    // server stop air TX (TX LED goes dark right when the client starts).
    if (!stream.peer_connected() && client_sock.fd() < 0 && !connecting)
    {
        stream.reset();
    }
}

bool tcp_client_endpoint::is_connecting() const
{
    return connecting;
}

void tcp_client_endpoint::on_client_write()
{
    if (connecting)
    {
        on_connecting();
        return;
    }
    tcp_endpoint::on_client_write();
}

void tcp_client_endpoint::close_client()
{
    tcp_endpoint::close_client();
    connecting = false;
}

bool tcp_client_endpoint::start_connect()
{
    if (client_sock.fd() >= 0 || connecting)
    {
        return true;
    }
    sockaddr_in addr = {};
    if (!parse_host_port(cfg.connect_address, &addr))
    {
        LOG_ERR("tcp connect_address invalid");
        return false;
    }
    client_sock = make_tcp4();
    if (client_sock.fd() < 0)
    {
        return false;
    }
    apply_buffers(client_sock);
    const int rv = client_sock.connect(addr);
    if (rv < 0 && errno != EINPROGRESS)
    {
        LOG_ERR("tcp connect %s: %s", cfg.connect_address.c_str(),
                strerror(errno));
        close_client();
        return false;
    }
    connecting = true;
    if (!watch_client_write())
    {
        close_client();
        return false;
    }
    return reactor->req_write(client_sock.fd());
}

void tcp_client_endpoint::on_connecting()
{
    if (client_sock.fd() < 0)
    {
        return;
    }
    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(client_sock.fd(), SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0)
    {
        LOG_WRN("tcp connect %s failed: %s (retry on next peer CONNECT)",
                cfg.connect_address.c_str(), strerror(err));
        close_client();
        return;
    }
    connecting = false;
    stream.local_up();
    if (!start_rx_thread())
    {
        close_client();
        return;
    }
    LOG_INF("tcp connected %s", cfg.connect_address.c_str());
    kick_tx();
    on_client_write();
}
