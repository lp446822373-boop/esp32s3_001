idf_component_register(SRCS "terminal.c" "wifi_prov.c" "status_led.c" "scanner.c" "main.c" "player.c" "button.c"
                            "terminal_config.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_timer lwip
                                  esp_driver_gpio esp_driver_uart espressif__led_strip
                                  esp_http_client espressif__cjson mbedtls)
