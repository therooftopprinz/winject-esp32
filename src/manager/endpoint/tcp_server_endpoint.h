#ifndef WINJECT_MANAGER_TCP_SERVER_ENDPOINT_H_
#define WINJECT_MANAGER_TCP_SERVER_ENDPOINT_H_

#include "endpoint/tcp_endpoint.h"

class tcp_server_endpoint : public tcp_endpoint
{
public:
    tcp_server_endpoint() = default;
    ~tcp_server_endpoint() override;
    bool open(::reactor& reactor, const upstream_config_s& cfg) override;
    void close() override;

protected:
    void on_peer_end() override;

private:
    void on_listen();

    bfc::socket listen_sock;
};

#endif  // WINJECT_MANAGER_TCP_SERVER_ENDPOINT_H_
