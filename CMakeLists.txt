idf_component_register(SRCS "api.c" "proto.c" "storage.c" "wifi_prov.c" "wifi_mgr.c" "web_srv.c" "dns_captive.c" "adc.c" "main.c"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES
                        spi_flash
                        nvs_flash
                        esp_event
                        esp_netif
                        esp_wifi
                        esp_http_server
                        lwip
                        esp_timer
                        esp_adc
                        driver
                        )
