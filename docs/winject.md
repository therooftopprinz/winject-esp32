# WInject-ESP32

WInject-ESP is an implementation of bfc-tunnel external multicast.
It will configure the esp32 wifi to monitor and inject raw packets. 

# Control Plane Console
Commands:
- `set_upstream_rx <port>` - listen to this udp port forward udp payload as raw wifi packet. The upstream is responsible for seting up 802.11 frames
- `set_upstream_tx <host> <port>` - every packet received in wifi will be forwarded to this host.+
 
- `set_channel <channel>` - Set WIFI channel
- `set_modulation <modulation>` - Set WIFI modulation

| Modulation Code | Modulation | Data Rate |
|-----------------|------------|-----------|
| DSS_1M_L | DBPSK (Long Preamble) | 1 Mbps |
| DSS_2M_S | DQPSK (Short Preamble) | 2 Mbps |
| DSS_2M_L | DQPSK (Long Preamble) | 2 Mbps |
| CCK_5M_L | CCK (Long Preamble) | 5.5 Mbps |
| CCK_5M_S | CCK (Short Preamble) | 5.5 Mbps |
| CCK_11M_L | CCK (Long Preamble) | 11 Mbps |
| CCK_11M_S | CCK (Short Preamble) | 11 Mbps |
| OFDM_6M | BPSK | 6 Mbps |
| OFDM_9M | BPSK | 9 Mbps |
| OFDM_12M | QPSK | 12 Mbps |
| OFDM_18M | QPSK | 18 Mbps |
| OFDM_24M | 16-QAM | 24 Mbps |
| OFDM_36M | 16-QAM | 36 Mbps |
| OFDM_48M | 64-QAM | 48 Mbps |
| OFDM_54M | 64-QAM | 54 Mbps |
| OFDM_MCS0_LGI | BPSK | 6.5 Mbps (20MHz), 13.5 Mbps (40MHz) |
| OFDM_MCS1_LGI | QPSK | 13.0 Mbps (20MHz), 27.0 Mbps (40MHz) |
| OFDM_MCS2_LGI | QPSK | 19.5 Mbps (20MHz), 40.5 Mbps (40MHz) |
| OFDM_MCS3_LGI | 16-QAM | 26.0 Mbps (20MHz), 54.0 Mbps (40MHz) |
| OFDM_MCS4_LGI | 16-QAM | 39.0 Mbps (20MHz), 81.0 Mbps (40MHz) |
| OFDM_MCS5_LGI | 64-QAM | 52.0 Mbps (20MHz), 108.0 Mbps (40MHz) |
| OFDM_MCS6_LGI | 64-QAM | 58.5 Mbps (20MHz), 121.5 Mbps (40MHz) |
| OFDM_MCS7_LGI | 64-QAM | 65.0 Mbps (20MHz), 135.0 Mbps (40MHz) |
| OFDM_MCS0_SGI | BPSK | 7.2 Mbps (20MHz), 15.0 Mbps (40MHz) |
| OFDM_MCS1_SGI | QPSK | 14.4 Mbps (20MHz), 30.0 Mbps (40MHz) |
| OFDM_MCS2_SGI | QPSK | 21.7 Mbps (20MHz), 45.0 Mbps (40MHz) |
| OFDM_MCS3_SGI | 16-QAM | 28.9 Mbps (20MHz), 60.0 Mbps (40MHz) |
| OFDM_MCS4_SGI | 16-QAM | 43.3 Mbps (20MHz), 90.0 Mbps (40MHz) |
| OFDM_MCS5_SGI | 64-QAM | 57.8 Mbps (20MHz), 120.0 Mbps (40MHz) |
| OFDM_MCS6_SGI | 64-QAM | 65.0 Mbps (20MHz), 135.0 Mbps (40MHz) |
| OFDM_MCS7_SGI | 64-QAM | 72.2 Mbps (20MHz), 150.0 Mbps (40MHz) |

- `set_cca_enabled` <is_enabled> - Set CCA to enabled