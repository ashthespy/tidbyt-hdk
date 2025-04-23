idf_component_register(
  SRCS         "ota_server.c"
  INCLUDE_DIRS "include"
  REQUIRES esp_http_server esp_http_client esp_https_ota app_update
)
# force-enable HTTP (no TLS) in esp_https_ota
target_compile_definitions(${COMPONENT_LIB}
    PUBLIC
        CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=1
)