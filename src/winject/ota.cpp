#include "ota.h"

#include "config.h"
#include "ethernet.h"
#include "wifi_radio.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "ota";
static httpd_handle_t g_httpd = nullptr;

static void ipv4ToString(uint32_t addr, char* out, size_t outLen)
{
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&addr);
  snprintf(out, outLen, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static esp_err_t handleRoot(httpd_req_t* req)
{
  char ethStr[16] = "down";
  char apStr[16] = "down";
  uint32_t ip = 0;
  if (ethernetLocalIpv4(&ip))
  {
    ipv4ToString(ip, ethStr, sizeof(ethStr));
  }
  if (wifiRadioApIpv4(&ip))
  {
    ipv4ToString(ip, apStr, sizeof(apStr));
  }

  char page[640];
  snprintf(page, sizeof(page),
           "<!DOCTYPE html><html><body>"
           "<h1>WInject-ESP32</h1>"
           "<p>eth %s</p>"
           "<p>ap %s %s</p>"
           "<p>HTTP POST /update</p>"
           "<form method='POST' action='/update' enctype='multipart/form-data'>"
           "<input type='file' name='firmware'>"
           "<input type='submit' value='Update'>"
           "</form></body></html>",
           ethStr, wifiRadioApActive() ? wifiRadioApSsid() : "-", apStr);
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static bool multipartBoundary(httpd_req_t* req, char* marker, size_t markerLen)
{
  char type[160] = {};
  if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) !=
      ESP_OK)
  {
    return false;
  }
  const char* found = nullptr;
  for (const char* p = type; *p != '\0'; p++)
  {
    if (strncasecmp(p, "boundary=", 9) == 0)
    {
      found = p;
      break;
    }
  }
  if (found == nullptr)
  {
    return false;
  }
  found += 9;
  if (*found == '"')
  {
    found++;
  }
  char token[72] = {};
  size_t i = 0;
  while (*found && *found != '"' && *found != ';' && *found != ' ' &&
         i + 1 < sizeof(token))
  {
    token[i++] = *found++;
  }
  if (i == 0)
  {
    return false;
  }
  snprintf(marker, markerLen, "\r\n--%s", token);
  return true;
}

static const uint8_t* findBytes(const uint8_t* data, size_t len,
                                const char* pat, size_t patLen)
{
  if (patLen == 0 || len < patLen)
  {
    return nullptr;
  }
  for (size_t i = 0; i + patLen <= len; i++)
  {
    if (memcmp(data + i, pat, patLen) == 0)
    {
      return data + i;
    }
  }
  return nullptr;
}

static esp_err_t handleUpdate(httpd_req_t* req)
{
  const esp_partition_t* update = esp_ota_get_next_update_partition(nullptr);
  if (update == nullptr)
  {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "no ota partition\n");
    return ESP_FAIL;
  }

  esp_ota_handle_t ota = 0;
  if (esp_ota_begin(update, OTA_SIZE_UNKNOWN, &ota) != ESP_OK)
  {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "ota begin failed\n");
    return ESP_FAIL;
  }

  char boundary[80] = {};
  const bool multipart = multipartBoundary(req, boundary, sizeof(boundary));
  const size_t boundaryLen = strlen(boundary);

  uint8_t buf[1024];
  uint8_t head[1024];
  size_t headLen = 0;
  bool inBody = !multipart;
  uint8_t tail[80];
  size_t tailLen = 0;
  int remaining = req->content_len;
  bool ok = true;
  bool sawEnd = !multipart;

  while (remaining > 0 && ok)
  {
    const int want = remaining < static_cast<int>(sizeof(buf))
                         ? remaining
                         : static_cast<int>(sizeof(buf));
    const int n = httpd_req_recv(req, reinterpret_cast<char*>(buf), want);
    if (n <= 0)
    {
      ok = false;
      break;
    }
    remaining -= n;
    const uint8_t* p = buf;
    size_t left = static_cast<size_t>(n);

    if (!inBody)
    {
      if (headLen + left > sizeof(head))
      {
        ok = false;
        break;
      }
      memcpy(head + headLen, p, left);
      headLen += left;
      const uint8_t* blank = findBytes(head, headLen, "\r\n\r\n", 4);
      if (blank == nullptr)
      {
        continue;
      }
      inBody = true;
      const size_t skip = static_cast<size_t>(blank - head) + 4;
      p = head + skip;
      left = headLen - skip;
    }

    if (left == 0)
    {
      continue;
    }

    if (!multipart)
    {
      if (esp_ota_write(ota, p, left) != ESP_OK)
      {
        ok = false;
      }
      continue;
    }

    uint8_t window[1024 + 80];
    memcpy(window, tail, tailLen);
    memcpy(window + tailLen, p, left);
    const size_t winLen = tailLen + left;
    const uint8_t* bound = findBytes(window, winLen, boundary, boundaryLen);
    if (bound != nullptr)
    {
      const size_t dataLen = static_cast<size_t>(bound - window);
      if (dataLen > 0 && esp_ota_write(ota, window, dataLen) != ESP_OK)
      {
        ok = false;
      }
      sawEnd = true;
      break;
    }
    const size_t keep = boundaryLen > 1 ? boundaryLen - 1 : 0;
    if (winLen > keep)
    {
      if (esp_ota_write(ota, window, winLen - keep) != ESP_OK)
      {
        ok = false;
        break;
      }
      memcpy(tail, window + (winLen - keep), keep);
      tailLen = keep;
    }
    else
    {
      memcpy(tail, window, winLen);
      tailLen = winLen;
    }
  }

  if (!ok || (multipart && !sawEnd))
  {
    esp_ota_abort(ota);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "update failed\n");
    return ESP_FAIL;
  }

  if (esp_ota_end(ota) != ESP_OK ||
      esp_ota_set_boot_partition(update) != ESP_OK)
  {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "update failed\n");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "HTTP OTA %s", update->label);
  httpd_resp_sendstr(req, "ok, rebooting\n");
  vTaskDelay(pdMS_TO_TICKS(200));
  esp_restart();
  return ESP_OK;
}

void otaBegin()
{
  if (g_httpd != nullptr)
  {
    return;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = OTA_HTTP_PORT;
  config.core_id = APP_TASK_CORE;
  config.stack_size = 8192;
  config.lru_purge_enable = true;
  if (httpd_start(&g_httpd, &config) != ESP_OK)
  {
    ESP_LOGE(TAG, "httpd start failed");
    g_httpd = nullptr;
    return;
  }

  const httpd_uri_t root = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = handleRoot,
      .user_ctx = nullptr,
  };
  const httpd_uri_t update = {
      .uri = "/update",
      .method = HTTP_POST,
      .handler = handleUpdate,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(g_httpd, &root);
  httpd_register_uri_handler(g_httpd, &update);
  ESP_LOGI(TAG, "HTTP :%u/update", OTA_HTTP_PORT);
}

bool otaActive()
{
  return g_httpd != nullptr;
}
