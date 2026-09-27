#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arpa/inet.h"
#include "dhcpserver/dhcpserver.h"
#include "dhcpserver/dhcpserver_options.h"
#include "driver/gpio.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "ra01s.h"

static const char* TAG = "CASTOR_GW";

#pragma region Configuration
#define STA_SSID "ATT59MhrJq"
#define STA_PASSWORD "t8+2apu9tdd#"

// Local network the gateway stands up
#define AP_SSID "CASTOR-GATEWAY"
#define AP_PASSWORD "castor-relief"  // SHould get rid of
#define AP_CHANNEL 6
#define AP_MAX_CONN 8

#define ENABLE_EMS_FORWARDING 1
#define EMS_ENDPOINT_URL \
  "https://webhook.site/d7ff6cd0-6971-4d9c-a34e-fea1511bbe3f"

static bool wifi_is_connected = false;
static esp_netif_t* ap_netif = NULL;

const char* hostname = "castor-node";

#pragma endregion  // Configuration

#pragma region Node role state machine

typedef enum {
  ROLE_ACCESS = 0,
  ROLE_UPLINK = 1,
} node_role_t;

static node_role_t current_role = ROLE_ACCESS;

static httpd_handle_t gw_http_server = NULL;

static void become_uplink(void);
static void remain_access(void);
static void disable_access_services(void);

static httpd_handle_t start_gateway_webserver(void);
static void stop_gateway_webserver(httpd_handle_t server);
static esp_err_t captive_portal_redirect_handler(httpd_req_t* req);

#pragma endregion  // Node role state machine

#pragma region Node identity & packet definitions
uint16_t NODE_ID;

void init_node_id(void) {
  NODE_ID = (uint16_t)(((esp_random() % 10000) + 1) & 0xFFFF);
  printf("Node ID: %u\n", NODE_ID);
}

typedef enum {
  PACKET_TEXT = 0,
  PACKET_LOCATION = 1,
  PACKET_IMAGE = 2,
  PACKET_SOS = 3,  // No more packet for gateway beacon, no need anymore
} PacketType;

typedef struct {
  uint16_t source_id;
  uint32_t packet_id;
  uint8_t ttl;
  uint8_t type;
  char message[48];
} PacketStruct;

uint8_t rx_buffer[256];

#define LORA_TX_QUEUE_SIZE 16
static QueueHandle_t lora_tx_queue = NULL;

#pragma endregion  // Node identity & packet definitions

#pragma region GPIO handling

#define BUTTON_GPIO 0

void GPIO_init(gpio_num_t gpio, gpio_mode_t mode, bool pullup, bool pulldown) {
  gpio_config_t config = {
      .pin_bit_mask = (1ULL << gpio),
      .mode = mode,
      .pull_up_en = pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
      .pull_down_en = pulldown ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE};
  gpio_config(&config);
}

void wait_for_button_release(gpio_num_t button) {
  while (gpio_get_level(button) == 0) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

#pragma endregion  // GPIO handling

#pragma region LoRa messages

void LoRa_setup(void) {
  LoRaInit();
  LoRaDebugPrint(true);
  LoRaBegin(915000000, 22, 3.3, false);
  LoRaConfig(7, SX126X_LORA_BW_125_0, SX126X_LORA_CR_4_5, 8, 60, true, false);
  printf("LoRa transceiver node ready\n");
}

void send_message(const char* message, uint16_t ID, uint8_t TTL, int PACKET_ID,
                  PacketType TYPE) {
  PacketStruct packet = {.source_id = ID, .ttl = TTL};
  packet.packet_id = (PACKET_ID == 0) ? esp_random() : (uint32_t)PACKET_ID;
  packet.type = TYPE;

  strncpy(packet.message, message, sizeof(packet.message) - 1);
  packet.message[sizeof(packet.message) - 1] = '\0';

  printf("\n--- Sending Packet ---\n");
  printf("Source : %u\n", packet.source_id);
  printf("Packet : %lu\n", (unsigned long)packet.packet_id);
  printf("TTL    : %u\n", packet.ttl);
  printf("Type   : %u\n", packet.type);
  printf("Text   : %s\n", packet.message);

  if (xQueueSend(lora_tx_queue, &packet, pdMS_TO_TICKS(100)) != pdTRUE) {
    printf("LoRa TX queue full — packet dropped\n");
  }
}

static void lora_tx_task(void* pvParameters) {
  PacketStruct packet;

  while (1) {
    if (xQueueReceive(lora_tx_queue, &packet, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    printf("\n--- LoRa TX Task ---\n");
    printf("Source : %u\n", packet.source_id);
    printf("Packet : %lu\n", (unsigned long)packet.packet_id);
    printf("TTL    : %u\n", packet.ttl);
    printf("Type   : %u\n", packet.type);
    printf("Text   : %s\n", packet.message);

    bool sent =
        LoRaSend((uint8_t*)&packet, sizeof(PacketStruct), SX126x_TXMODE_SYNC);

    printf(sent ? "Packet sent\n\n" : "Packet failed\n\n");

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

typedef struct {
  uint16_t source_id;
  uint8_t type;
  char message[48];
} EmsForwardMsg;

static QueueHandle_t ems_forward_queue = NULL;

static void ems_forward_task(void* pvParameters) {
  EmsForwardMsg msg;
  while (1) {
    if (xQueueReceive(ems_forward_queue, &msg, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    char body[192];
    snprintf(body, sizeof(body),
             "{\"source_id\":%u,\"type\":%u,\"message\":\"%s\"}", msg.source_id,
             msg.type, msg.message);

    esp_http_client_config_t config = {
        .url = EMS_ENDPOINT_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "EMS forward OK, status=%d",
               esp_http_client_get_status_code(client));
    } else {
      ESP_LOGW(TAG, "EMS forward failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
  }
}

// Sends to EMS and through network if wifi/gateway
static void forward_to_ems(const PacketStruct* packet) {
  if (current_role != ROLE_UPLINK || !wifi_is_connected) {
    return;
  }

  ESP_LOGI(TAG, "EMS forward candidate: node %u type %u msg \"%s\"",
           packet->source_id, packet->type, packet->message);

#if ENABLE_EMS_FORWARDING
  if (ems_forward_queue == NULL) return;

  EmsForwardMsg msg = {.source_id = packet->source_id, .type = packet->type};
  strncpy(msg.message, packet->message, sizeof(msg.message) - 1);
  msg.message[sizeof(msg.message) - 1] = '\0';

  if (xQueueSend(ems_forward_queue, &msg, 0) != pdTRUE) {
    ESP_LOGW(TAG, "EMS forward queue full, dropping message");
  }
#endif
}

/* Packet cacheing so packets already get seen dies*/

#define PACKET_CACHE_SIZE 32

typedef struct {
  uint16_t source_id;
  uint32_t packet_id;
} PacketCacheEntry;

static PacketCacheEntry packet_cache[PACKET_CACHE_SIZE];
static uint16_t packet_cache_index = 0;

static bool packet_already_seen(uint16_t source_id, uint32_t packet_id) {
  for (int i = 0; i < PACKET_CACHE_SIZE; i++) {
    if (packet_cache[i].source_id == source_id &&
        packet_cache[i].packet_id == packet_id) {
      return true;
    }
  }
  return false;
}

static void packet_cache_remember(uint16_t source_id, uint32_t packet_id) {
  packet_cache[packet_cache_index].source_id = source_id;
  packet_cache[packet_cache_index].packet_id = packet_id;
  packet_cache_index = (packet_cache_index + 1) % PACKET_CACHE_SIZE;
}

static void originate_message(const char* message, PacketType type) {
  uint32_t packet_id = esp_random();
  if (packet_id == 0) packet_id = 1;  // reserve 0 as "let send_message pick"
  packet_cache_remember(NODE_ID, packet_id);

  send_message(message, NODE_ID, 5, packet_id, type);

  if (type == PACKET_SOS || type == PACKET_LOCATION) {
    PacketStruct local_packet = {
        .source_id = NODE_ID,
        .packet_id = packet_id,
        .ttl = 5,
        .type = (uint8_t)type,
    };
    strncpy(local_packet.message, message, sizeof(local_packet.message) - 1);
    local_packet.message[sizeof(local_packet.message) - 1] = '\0';
    forward_to_ems(&local_packet);
  }
}

void handle_receive(void) {
  uint8_t received = LoRaReceive(rx_buffer, sizeof(rx_buffer));
  if (received == 0) return;

  if (received != sizeof(PacketStruct)) {
    printf("Invalid packet size (%u bytes)\n", received);
    return;
  }

  PacketStruct packet;
  memcpy(&packet, rx_buffer, sizeof(PacketStruct));

  if (packet_already_seen(packet.source_id, packet.packet_id)) {
    printf("Duplicate packet ignored (source %u, id %lu)\n", packet.source_id,
           (unsigned long)packet.packet_id);
    return;
  }
  packet_cache_remember(packet.source_id, packet.packet_id);

  printf("\n--- Received Packet ---\n");
  printf("Source : %u\n", packet.source_id);
  printf("Packet : %lu\n", (unsigned long)packet.packet_id);
  printf("TTL    : %u\n", packet.ttl);
  printf("Type   : %u\n", packet.type);

  switch (packet.type) {
    case PACKET_TEXT:
      printf("Message: %s\n", packet.message);
      break;
    case PACKET_LOCATION:
      printf("Location packet received\n");
      forward_to_ems(&packet);
      break;
    case PACKET_SOS:
      printf("SOS packet received: %s\n", packet.message);
      forward_to_ems(&packet);
      break;
    case PACKET_IMAGE:
      printf("Image packet received\n");
      break;
    default:
      printf("Unknown packet type\n");
      break;
  }

  printf("-----------------------\n");
  if (packet.ttl > 0) {
    printf("TTL subtracted by one and packet forwarded.\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    send_message(packet.message, packet.source_id, packet.ttl - 1,
                 packet.packet_id, packet.type);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

#pragma endregion  // LoRa messages

#pragma region HTTP server

// clang-format off
static const char* GATEWAY_FORM_HTML =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
        "<title>CASTOR</title>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "</head>"
    "<body style='font-family:sans-serif;max-width:480px;margin:2em auto'>"
        "<h2>CASTOR Emergency Relay</h2>"
        "<p>Connected to gateway node. Messages sent here go out over the mesh.</p>"
        "<form method='POST' action='/send'>"
            "<label>Type:<br>"
            "<select name='type'>"
                "<option value='3'>SOS</option>"
                "<option value='1'>Location</option>"
                "<option value='0'>General message</option>"
            "</select></label><br><br>"
            "<label>Message:<br>"
            "<textarea name='message' rows='3' style='width:100%'></textarea></label><br><br>"
            "<button type='submit' style='padding:0.75em 1.5em'>Send</button>"
        "</form>"
    "</body>"
    "</html>";
// clang-format on

static esp_err_t root_get_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, GATEWAY_FORM_HTML, HTTPD_RESP_USE_STRLEN);
}

// Extremely small form-urlencoded parser: good enough for our two fields,
// not a general-purpose decoder. REVIEW: swap for something more robust
// (e.g. percent-decoding) before treating this as production input handling.
static bool extract_form_value(const char* body, const char* key, char* out,
                               size_t out_len) {
  char needle[32];
  snprintf(needle, sizeof(needle), "%s=", key);
  const char* start = strstr(body, needle);
  if (!start) return false;
  start += strlen(needle);
  const char* end = strchr(start, '&');
  size_t len = end ? (size_t)(end - start) : strlen(start);
  if (len >= out_len) len = out_len - 1;
  memcpy(out, start, len);
  out[len] = '\0';
  // Minimal cleanup: '+' -> space (skips full percent-decoding for brevity)
  for (size_t i = 0; i < len; i++) {
    if (out[i] == '+') out[i] = ' ';
  }
  return true;
}

static esp_err_t send_post_handler(httpd_req_t* req) {
  char body[256] = {0};
  int total_len = req->content_len;
  if (total_len <= 0 || total_len >= (int)sizeof(body)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
    return ESP_FAIL;
  }
  int received = httpd_req_recv(req, body, total_len);
  if (received <= 0) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
    return ESP_FAIL;
  }
  body[received] = '\0';

  char type_str[8] = "0";
  char message[64] = "SOS from phone";
  extract_form_value(body, "type", type_str, sizeof(type_str));
  extract_form_value(body, "message", message, sizeof(message));

  PacketType type = (PacketType)atoi(type_str);
  originate_message(message, type);

  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(
      req,
      "<html><body style='font-family:sans-serif'>"
      "<p>Sent onto the mesh.</p><a href='/'>Send another</a>"
      "</body></html>",
      HTTPD_RESP_USE_STRLEN);
}

static httpd_handle_t start_gateway_webserver(void) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.uri_match_fn = httpd_uri_match_wildcard;
  httpd_handle_t server = NULL;

  if (httpd_start(&server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start gateway HTTP server");
    return NULL;
  }

  httpd_uri_t root_uri = {
      .uri = "/", .method = HTTP_GET, .handler = root_get_handler};
  httpd_uri_t send_uri = {
      .uri = "/send", .method = HTTP_POST, .handler = send_post_handler};

  httpd_uri_t catchall_uri = {
      .uri = "/*",
      .method = HTTP_GET,
      .handler = captive_portal_redirect_handler,
  };
  httpd_register_uri_handler(server, &root_uri);
  httpd_register_uri_handler(server, &send_uri);
  httpd_register_uri_handler(server, &catchall_uri);

  ESP_LOGI(TAG, "Gateway HTTP server started");
  return server;
}

static void stop_gateway_webserver(httpd_handle_t server) {
  if (server) {
    httpd_stop(server);
    ESP_LOGI(TAG, "Gateway HTTP server stopped");
  }
}

#pragma endregion  // HTTP server

#pragma region Captive portal (DNS hijack + browser popup)

#define DNS_PORT 53
#define DNS_MAX_LEN 512

static TaskHandle_t dns_hijack_task_handle = NULL;

static void dns_hijack_task(void* pvParameters) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

  if (sock < 0) {
    ESP_LOGE(TAG, "DNS hijack: socket() failed");
    vTaskDelete(NULL);
    return;
  }

  struct sockaddr_in bind_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(DNS_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };

  if (bind(sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
    ESP_LOGE(TAG, "DNS hijack: bind() failed (port 53 busy?)");
    close(sock);
    vTaskDelete(NULL);
    return;
  }

  uint32_t ap_ip = inet_addr("192.168.4.1");

  uint8_t rx[DNS_MAX_LEN];
  uint8_t tx[DNS_MAX_LEN];

  ESP_LOGI(TAG, "DNS hijack server started");

  while (1) {
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);

    int len = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr*)&from_addr,
                       &from_len);

    if (len < 12) {
      continue;
    }

    int qname_end = 12;

    while (qname_end < len && rx[qname_end] != 0) {
      qname_end += rx[qname_end] + 1;
    }

    qname_end += 1;

    int question_end = qname_end + 4;

    if (question_end > len || question_end > DNS_MAX_LEN) {
      continue;
    }

    memcpy(tx, rx, question_end);

    tx[2] = 0x81;
    tx[3] = 0x80;

    tx[6] = 0x00;
    tx[7] = 0x01;

    int pos = question_end;

    tx[pos++] = 0xC0;
    tx[pos++] = 0x0C;

    tx[pos++] = 0x00;
    tx[pos++] = 0x01;

    tx[pos++] = 0x00;
    tx[pos++] = 0x01;

    tx[pos++] = 0x00;
    tx[pos++] = 0x00;
    tx[pos++] = 0x00;
    tx[pos++] = 0x3C;

    tx[pos++] = 0x00;
    tx[pos++] = 0x04;

    memcpy(&tx[pos], &ap_ip, 4);
    pos += 4;

    sendto(sock, tx, pos, 0, (struct sockaddr*)&from_addr, from_len);
  }
}

static esp_err_t captive_portal_redirect_handler(httpd_req_t* req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  return httpd_resp_send(req, NULL, 0);
}

#pragma endregion  // Captive portal (DNS hijack + browser popup)

#pragma region Role transitions

static void remain_access(void) {
  if (current_role == ROLE_ACCESS) {
    return;
  }

  ESP_LOGI(TAG, "Node operating as access node");

  current_role = ROLE_ACCESS;
}

static void become_uplink(void) {
  if (current_role == ROLE_UPLINK) {
    return;
  }

  ESP_LOGI(TAG, "Uplink acquired — node is now an uplink");

  current_role = ROLE_UPLINK;
}

#pragma endregion  // Role transitions

#pragma region WiFi

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
  if (event_base == WIFI_EVENT) {
    switch (event_id) {
      case WIFI_EVENT_STA_START:
        printf("Upstream Wi-Fi search started.\n");
        esp_wifi_connect();
        break;

      case WIFI_EVENT_STA_DISCONNECTED:
        wifi_is_connected = false;

        if (current_role == ROLE_UPLINK) {
          ESP_LOGW(TAG, "Upstream Wi-Fi lost — returning to access mode");
          remain_access();
        } else {
          ESP_LOGI(TAG, "No upstream Wi-Fi — operating as access node");
        }

        esp_wifi_connect();
        break;

      default:
        break;
    }
  } else if (event_base == IP_EVENT) {
    if (event_id == IP_EVENT_STA_GOT_IP) {
      wifi_is_connected = true;

      ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;

      printf("Upstream Wi-Fi connected! IP Address: " IPSTR "\n",
             IP2STR(&event->ip_info.ip));

      become_uplink();
    }
  }
}

void wifi_init(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  esp_netif_create_default_wifi_sta();
  ap_netif =
      esp_netif_create_default_wifi_ap();  // exists from boot; AP config/enable
                                           // only happens in become_gateway()

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &wifi_event_handler, NULL));

  wifi_config_t sta_config = {
      .sta =
          {
              .ssid = STA_SSID,
              .password = STA_PASSWORD,
          },
  };
  wifi_config_t ap_config = {
      .ap =
          {
              .ssid = AP_SSID,
              .ssid_len = strlen(AP_SSID),
              .password = AP_PASSWORD,
              .channel = AP_CHANNEL,
              .max_connection = AP_MAX_CONN,
              .authmode = WIFI_AUTH_WPA2_PSK,
          },
  };

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

  dhcps_offer_t dhcps_dns_value = OFFER_DNS;

  ESP_ERROR_CHECK(esp_netif_dhcps_stop(ap_netif));

  esp_err_t dns_err = esp_netif_dhcps_option(
      ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
      &dhcps_dns_value, sizeof(dhcps_dns_value));

  if (dns_err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to set AP DNS option: %s", esp_err_to_name(dns_err));
  }

  ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

  ESP_ERROR_CHECK(esp_wifi_start());

  gw_http_server = start_gateway_webserver();

  if (dns_hijack_task_handle == NULL) {
    xTaskCreate(dns_hijack_task, "dns_hijack", 4096, NULL, 5,
                &dns_hijack_task_handle);
  }

  current_role = ROLE_ACCESS;

  printf("CASTOR access network started.\n");
  printf("AP SSID: %s\n", AP_SSID);
  printf("AP IP: 192.168.4.1\n");
  printf("Attempting upstream Wi-Fi connection...\n");
}

#pragma endregion  // WiFi

#pragma region Main

void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  init_node_id();
  GPIO_init(BUTTON_GPIO, GPIO_MODE_INPUT, true, false);
  LoRa_setup();

  lora_tx_queue = xQueueCreate(LORA_TX_QUEUE_SIZE, sizeof(PacketStruct));
  if (lora_tx_queue == NULL) {
    ESP_LOGE(TAG, "Failed to create LoRa TX queue");
    abort();
  }

  xTaskCreate(lora_tx_task, "lora_tx", 4096, NULL, 6, NULL);

  wifi_init();

  // clang-format off
  #if ENABLE_EMS_FORWARDING
    ems_forward_queue = xQueueCreate(4, sizeof(EmsForwardMsg));
    xTaskCreate(ems_forward_task, "ems_forward", 8192, NULL, 5, NULL);
  #endif
  //clang-format on

  printf("sizeof(PacketStruct) = %u\n", (unsigned)sizeof(PacketStruct));

  while (1) {
    handle_receive();

    if (gpio_get_level(BUTTON_GPIO) == 0) {
      originate_message("SOS at this location", PACKET_SOS);
      wait_for_button_release(BUTTON_GPIO);
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

#pragma endregion  // Main