#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_log.h"

static const char *TAG = "OTA";

static esp_err_t ota_pull_handler(httpd_req_t *req)
{
    char query[128];
    httpd_req_get_url_query_str(req, query, sizeof(query));

    char md5str[33]    = {0};
    char path[64]      = {0};
    char host[40]      = {0};
    char port_str[8]   = {0};

    httpd_query_key_value(query, "MD5",  md5str,    sizeof(md5str));
    httpd_query_key_value(query, "path", path,      sizeof(path));
    httpd_query_key_value(query, "host", host,      sizeof(host));
    httpd_query_key_value(query, "port", port_str,  sizeof(port_str));

    int port = atoi(port_str);
    if (port <= 0) {
        ESP_LOGE(TAG, "Invalid port: '%s'", port_str);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad port");
        return ESP_FAIL;
    }

    char ota_url[256];
    snprintf(ota_url, sizeof(ota_url),
             "http://%s:%d%s", host, port, path);

    ESP_LOGI(TAG, "Pulling firmware from %s (MD5=%s)", ota_url, md5str);

    esp_http_client_config_t http_cfg = {
        .url            = ota_url,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
        ESP_LOGI(TAG, "Firmware update OK — rebooting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed (%d)", err);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA failed");
    }
    return ESP_OK;
}

esp_err_t start_ota_server(void) {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  esp_err_t err = httpd_start(&server, &config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
    return err;
  }
  httpd_uri_t up_uri = {.uri = "/",
                        .method = HTTP_GET,
                        .handler = ota_pull_handler,
                        .user_ctx = NULL};
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &up_uri));
  ESP_LOGI(TAG, "OTA server started..");
  return ESP_OK;
}
