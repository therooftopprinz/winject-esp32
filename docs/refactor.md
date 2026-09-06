# Refactor manager

TCP Dataflow:

tcp_endpoint --> tcp_stream --> tx_scheduler --> radio_tx
tcp_endpoint <-- tcp_stream <-- radio_rx


```uml
class tcp_stream:
+ void on_connect()
+ void on_send(uint8_t*, size_t)
+ void on_recv()
+ size_t write_pdu(uint8_t*, size_t)
```