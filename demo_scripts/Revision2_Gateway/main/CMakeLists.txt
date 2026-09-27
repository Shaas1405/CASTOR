idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS ""
                       REQUIRES
                            spi_flash
                            ra01s
                            esp_wifi
                            esp_event
                            esp_netif
                            nvs_flash
                            esp_http_server 
                            esp_http_client
                            lwip
                            mbedtls
                            )
