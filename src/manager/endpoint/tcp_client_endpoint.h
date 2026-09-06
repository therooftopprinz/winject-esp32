#ifndef WINJECT_MANAGER_TCP_CLIENT_ENDPOINT_H_
#define WINJECT_MANAGER_TCP_CLIENT_ENDPOINT_H_

#include "endpoint/tcp_endpoint.h"

class tcp_client_endpoint : public tcp_endpoint
{
public:
    tcp_client_endpoint() = default;
    ~tcp_client_endpoint() override;

protected:
    void on_peer_end() override;
    void maybe_connect() override;
    void on_incoming_connect() override;
    bool is_connecting() const override;
    void on_client_write() override;
    void close_client() override;

private:
    bool start_connect();
    void on_connecting();

    bool connecting = false;
};

#endif  // WINJECT_MANAGER_TCP_CLIENT_ENDPOINT_H_
