idf_component_register(SRCS "main.c"
                       REQUIRES spi_flash
                       INCLUDE_DIRS ""
                       REQUIRES ra01s
                       REQUIRES
                            esp_wifi
                            esp_event
                            esp_netif
                            nvs_flash
                            esp_http_server 
                            esp_http_client
                            lwip
                            mbedtls
                            )
