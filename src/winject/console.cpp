#include "console.h"

#include "config.h"
#include "ethernet.h"
#include "frame.h"
#include "ota.h"
#include "upstream.h"
#include "wifi_radio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char* TAG = "console";

static int g_listenFd = -1;
static int g_clientFd = -1;
static char g_serialLine[128];
static size_t g_serialLen = 0;
static char g_tcpLine[128];
static size_t g_tcpLen = 0;
static bool g_uartReady = false;

struct ConsoleOut
{
  int fd;
};

static ConsoleOut serialOut()
{
  return {-1};
}

static ConsoleOut tcpOut()
{
  return {g_clientFd};
}

static bool setNonblock(int fd)
{
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0)
  {
    return false;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void closeFd(int* fd)
{
  if (fd == nullptr || *fd < 0)
  {
    return;
  }
  close(*fd);
  *fd = -1;
}

static void closeClient()
{
  closeFd(&g_clientFd);
  g_tcpLen = 0;
}

static void consoleStopTcp()
{
  closeClient();
  closeFd(&g_listenFd);
}

static void ipv4ToString(uint32_t addr, char* out, size_t outLen)
{
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&addr);
  snprintf(out, outLen, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static void consoleWrite(const ConsoleOut& out, const char* text)
{
  if (text == nullptr || *text == '\0')
  {
    return;
  }
  if (out.fd < 0)
  {
    const size_t len = strlen(text);
    if (g_uartReady)
    {
      uart_write_bytes(UART_NUM_0, text, len);
    }
    else
    {
      fwrite(text, 1, len, stdout);
      fflush(stdout);
    }
    return;
  }
  if (out.fd != g_clientFd)
  {
    return;
  }

  size_t len = strlen(text);
  size_t off = 0;
  while (off < len)
  {
    const int n = send(out.fd, text + off, len - off, 0);
    if (n < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        return;
      }
      closeClient();
      return;
    }
    if (n == 0)
    {
      closeClient();
      return;
    }
    off += static_cast<size_t>(n);
  }
}

static void consolePrintf(const ConsoleOut& out, const char* fmt, ...)
{
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  consoleWrite(out, buf);
}

static bool parseBool(const char* text, bool* value)
{
  if (text == nullptr || value == nullptr)
  {
    return false;
  }
  if (strcasecmp(text, "1") == 0 || strcasecmp(text, "true") == 0 ||
      strcasecmp(text, "on") == 0 || strcasecmp(text, "yes") == 0)
  {
    *value = true;
    return true;
  }
  if (strcasecmp(text, "0") == 0 || strcasecmp(text, "false") == 0 ||
      strcasecmp(text, "off") == 0 || strcasecmp(text, "no") == 0)
  {
    *value = false;
    return true;
  }
  return false;
}

static bool parsePort(const char* text, uint16_t* port)
{
  if (text == nullptr || port == nullptr || *text == '\0')
  {
    return false;
  }
  char* end = nullptr;
  const long value = strtol(text, &end, 10);
  if (end == text || *end != '\0' || value <= 0 || value > 65535)
  {
    return false;
  }
  *port = static_cast<uint16_t>(value);
  return true;
}

static bool parseChannel(const char* text, uint8_t* channel)
{
  if (text == nullptr || channel == nullptr || *text == '\0')
  {
    return false;
  }
  char* end = nullptr;
  const long value = strtol(text, &end, 10);
  if (end == text || *end != '\0' || value < 1 || value > 13)
  {
    return false;
  }
  *channel = static_cast<uint8_t>(value);
  return true;
}

static bool parseHost(const char* text, uint32_t* ip)
{
  if (text == nullptr || ip == nullptr || *text == '\0')
  {
    return false;
  }
  struct in_addr addr = {};
  if (inet_pton(AF_INET, text, &addr) == 1)
  {
    *ip = addr.s_addr;
    return true;
  }

  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  struct addrinfo* res = nullptr;
  if (getaddrinfo(text, nullptr, &hints, &res) != 0 || res == nullptr)
  {
    return false;
  }
  const auto* sin = reinterpret_cast<const struct sockaddr_in*>(res->ai_addr);
  *ip = sin->sin_addr.s_addr;
  freeaddrinfo(res);
  return *ip != 0;
}

static void printMac(const ConsoleOut& out, const uint8_t mac[6])
{
  consolePrintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
                mac[3], mac[4], mac[5]);
}

static void printBanner(const ConsoleOut& out)
{
  consoleWrite(out, "WInject-ESP32  bfc-tunnel external multicast radio\n");
  consoleWrite(out, "type help\n");
}

static void printHelp(const ConsoleOut& out)
{
  consoleWrite(out, "set_upstream_rx <port>\n");
  consoleWrite(out, "set_upstream_tx <host> <port>\n");
  consoleWrite(out, "set_channel <channel>\n");
  consoleWrite(out, "set_modulation <modulation>\n");
  consoleWrite(out, "set_cca_enabled <0|1>\n");
  consoleWrite(out, "status\n");
  consoleWrite(out, "help\n");
  consoleWrite(out, "modulations: ");
  consoleWrite(out, wifiRadioModulationList());
  consoleWrite(out, "\n");
  consoleWrite(out, "ota: HTTP POST /update\n");
}

static void printStatus(const ConsoleOut& out)
{
  WifiRadioStatus radio = {};
  UpstreamStatus up = {};
  wifiRadioGetStatus(&radio);
  upstreamGetStatus(&up);

  uint32_t ethIp = 0;
  if (ethernetLocalIpv4(&ethIp))
  {
    char ipStr[16];
    ipv4ToString(ethIp, ipStr, sizeof(ipStr));
    consolePrintf(out, "eth %s  console %s:%u\n", ipStr, ipStr,
                  CONTROL_CONSOLE_PORT);
  }
  else
  {
    consoleWrite(out, "eth down\n");
  }

  if (wifiRadioApActive())
  {
    char apIp[16] = "-";
    uint32_t ip = 0;
    if (wifiRadioApIpv4(&ip))
    {
      ipv4ToString(ip, apIp, sizeof(apIp));
    }
    consolePrintf(out, "ap %s  %s  open\n", wifiRadioApSsid(), apIp);
  }

  if (otaActive())
  {
    consolePrintf(out, "ota http %u/update\n", OTA_HTTP_PORT);
  }
  else
  {
    consoleWrite(out, "ota waiting\n");
  }

  if (wifiRadioReady())
  {
    consolePrintf(out, "channel %u  modulation %s  cca %s\n", radio.channel,
                  radio.modulation, radio.ccaEnabled ? "enabled" : "disabled");

    uint8_t staMac[6] = {};
    uint8_t bssid[6] = {};
    const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    frameGetStaMac(staMac);
    frameGetBssid(bssid);
    consoleWrite(out, "sa ");
    printMac(out, staMac);
    consoleWrite(out, "  da ");
    printMac(out, broadcast);
    consoleWrite(out, "  bssid ");
    printMac(out, bssid);
    consoleWrite(out, "\n");
  }

  if (up.rxBound)
  {
    consolePrintf(out, "upstream_rx %u\n", up.rxPort);
  }
  else
  {
    consoleWrite(out, "upstream_rx unset\n");
  }

  if (up.txSet)
  {
    char hostStr[16];
    ipv4ToString(up.txHost, hostStr, sizeof(hostStr));
    consolePrintf(out, "upstream_tx %s:%u\n", hostStr, up.txPort);
  }
  else
  {
    consoleWrite(out, "upstream_tx unset\n");
  }

  consolePrintf(
      out, "wifi_rx %u  wifi_tx %u  wifi_rx_drop %u  wifi_tx_fail %u\n",
      radio.wifiRx, radio.wifiTx, radio.wifiRxDropped, radio.wifiTxFail);
  consolePrintf(out, "udp_rx %u  udp_tx %u  udp_rx_drop %u\n", up.udpRx,
                up.udpTx, up.udpRxDropped);
}

static void handleLine(const char* line, const ConsoleOut& out)
{
  char copy[128];
  strncpy(copy, line, sizeof(copy) - 1);
  copy[sizeof(copy) - 1] = '\0';

  char* save = nullptr;
  char* cmd = strtok_r(copy, " \t", &save);
  if (cmd == nullptr || *cmd == '\0' || cmd[0] == '#')
  {
    return;
  }

  if (strcasecmp(cmd, "help") == 0 || strcasecmp(cmd, "?") == 0)
  {
    printHelp(out);
    return;
  }
  if (strcasecmp(cmd, "status") == 0)
  {
    printStatus(out);
    return;
  }

  char* arg1 = strtok_r(nullptr, " \t", &save);
  char* arg2 = strtok_r(nullptr, " \t", &save);

  if (strcasecmp(cmd, "set_upstream_rx") == 0)
  {
    uint16_t port = 0;
    if (!parsePort(arg1, &port) || arg2 != nullptr)
    {
      consoleWrite(out, "error: usage set_upstream_rx <port>\n");
      return;
    }
    if (!ethernetConnected())
    {
      consoleWrite(out, "error: ethernet is down\n");
      return;
    }
    if (!upstreamSetRxPort(port))
    {
      consoleWrite(out, "error: failed to bind udp port\n");
      return;
    }
    consoleWrite(out, "ok\n");
    return;
  }

  if (strcasecmp(cmd, "set_upstream_tx") == 0)
  {
    uint32_t host = 0;
    uint16_t port = 0;
    if (!parseHost(arg1, &host) || !parsePort(arg2, &port))
    {
      consoleWrite(out, "error: usage set_upstream_tx <host> <port>\n");
      return;
    }
    if (!ethernetConnected())
    {
      consoleWrite(out, "error: ethernet is down\n");
      return;
    }
    if (!upstreamSetTx(host, port))
    {
      consoleWrite(out, "error: failed to set tx destination\n");
      return;
    }
    consoleWrite(out, "ok\n");
    return;
  }

  if (strcasecmp(cmd, "set_channel") == 0)
  {
    uint8_t channel = 0;
    if (!parseChannel(arg1, &channel) || arg2 != nullptr)
    {
      consoleWrite(out, "error: usage set_channel <1-13>\n");
      return;
    }
    if (!wifiRadioSetChannel(channel))
    {
      consoleWrite(out, "error: failed to set channel\n");
      return;
    }
    consoleWrite(out, "ok\n");
    return;
  }

  if (strcasecmp(cmd, "set_modulation") == 0)
  {
    if (arg1 == nullptr || arg2 != nullptr)
    {
      consoleWrite(out, "error: usage set_modulation <modulation>\n");
      return;
    }
    if (!wifiRadioSetModulation(arg1))
    {
      consoleWrite(out, "error: unknown modulation\n");
      return;
    }
    consoleWrite(out, "ok\n");
    return;
  }

  if (strcasecmp(cmd, "set_cca_enabled") == 0)
  {
    bool enabled = false;
    if (!parseBool(arg1, &enabled) || arg2 != nullptr)
    {
      consoleWrite(out, "error: usage set_cca_enabled <0|1>\n");
      return;
    }
    if (!wifiRadioSetCcaEnabled(enabled))
    {
      consoleWrite(out, "error: failed to set cca\n");
      return;
    }
    consoleWrite(out, "ok\n");
    return;
  }

  consoleWrite(out, "error: unknown command, type help\n");
}

static void feedChar(char c, char* line, size_t* len, size_t maxLen,
                     const ConsoleOut& out)
{
  if (c == '\r')
  {
    return;
  }
  if (c == '\n')
  {
    line[*len] = '\0';
    *len = 0;
    handleLine(line, out);
    return;
  }
  if (*len + 1 >= maxLen)
  {
    *len = 0;
    consoleWrite(out, "error: line too long\n");
    return;
  }
  line[(*len)++] = c;
}

static void consoleStartTcp()
{
  if (g_listenFd >= 0)
  {
    return;
  }

  const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0)
  {
    ESP_LOGE(TAG, "socket failed: %d", errno);
    return;
  }

  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(CONTROL_CONSOLE_PORT);
  addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
  {
    ESP_LOGE(TAG, "bind *:%u failed: %d", CONTROL_CONSOLE_PORT, errno);
    close(fd);
    return;
  }
  if (listen(fd, 2) < 0 || !setNonblock(fd))
  {
    ESP_LOGE(TAG, "listen failed: %d", errno);
    close(fd);
    return;
  }

  g_listenFd = fd;

  uint32_t ip = 0;
  if (ethernetLocalIpv4(&ip))
  {
    char ipStr[16];
    ipv4ToString(ip, ipStr, sizeof(ipStr));
    ESP_LOGI(TAG, "control console on %s:%u", ipStr, CONTROL_CONSOLE_PORT);
  }
  if (wifiRadioApIpv4(&ip))
  {
    char ipStr[16];
    ipv4ToString(ip, ipStr, sizeof(ipStr));
    ESP_LOGI(TAG, "control console on %s:%u (ap)", ipStr, CONTROL_CONSOLE_PORT);
  }
}

static void acceptClient()
{
  struct sockaddr_in peer = {};
  socklen_t peerLen = sizeof(peer);
  const int fd =
      accept(g_listenFd, reinterpret_cast<struct sockaddr*>(&peer), &peerLen);
  if (fd < 0)
  {
    return;
  }

  closeClient();
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  if (!setNonblock(fd))
  {
    close(fd);
    return;
  }
  g_clientFd = fd;
  printBanner(tcpOut());
}

static void pollClient()
{
  char buf[64];
  while (g_clientFd >= 0)
  {
    const int n = recv(g_clientFd, buf, sizeof(buf), 0);
    if (n < 0)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
      {
        return;
      }
      closeClient();
      return;
    }
    if (n == 0)
    {
      closeClient();
      return;
    }
    const ConsoleOut out = tcpOut();
    for (int i = 0; i < n && g_clientFd >= 0; i++)
    {
      feedChar(buf[i], g_tcpLine, &g_tcpLen, sizeof(g_tcpLine), out);
    }
  }
}

static void pollUart()
{
  if (!g_uartReady)
  {
    return;
  }
  uint8_t buf[32];
  const int n = uart_read_bytes(UART_NUM_0, buf, sizeof(buf), 0);
  for (int i = 0; i < n; i++)
  {
    feedChar(static_cast<char>(buf[i]), g_serialLine, &g_serialLen,
             sizeof(g_serialLine), serialOut());
  }
}

static void consoleTask(void* arg)
{
  (void)arg;
  printBanner(serialOut());
  for (;;)
  {
    if (ethernetConnected() || wifiRadioApActive())
    {
      consoleStartTcp();
    }
    else
    {
      consoleStopTcp();
    }

    pollUart();
    if (g_listenFd >= 0)
    {
      acceptClient();
      pollClient();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void consoleBegin()
{
  if (uart_driver_install(UART_NUM_0, 512, 0, 0, nullptr, 0) == ESP_OK)
  {
    g_uartReady = true;
  }
  else
  {
    ESP_LOGW(TAG, "UART0 driver install failed, using stdout");
  }

  xTaskCreatePinnedToCore(consoleTask, "console", 4096, nullptr,
                          CONSOLE_TASK_PRIO, nullptr, APP_TASK_CORE);
}
