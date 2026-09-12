#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ra01s.h"

#define IS_WIFI_GATEWAY 1
#define WIFI_SSID "ATT59MhrJq"
#define WIFI_PASSWORD "t8+2apu9tdd#"
static bool wifi_is_connected = false;
const char* hostname = "esp32-custom-name";

#pragma region WiFi

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
  if (event_base == WIFI_EVENT) {
    switch (event_id) {
      case WIFI_EVENT_STA_START:
        printf("Wi-Fi started, connecting to AP...\n");
        esp_wifi_connect();
        break;
      case WIFI_EVENT_STA_DISCONNECTED:
        wifi_is_connected = false;
        printf("Wi-Fi disconnected... retrying connection\n");
        esp_wifi_connect();
        break;
      default:
        break;
    }
  } else if (event_base == IP_EVENT) {
    switch (event_id) {
      case IP_EVENT_STA_GOT_IP: {
        wifi_is_connected = true;
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        printf("Wi-Fi connected! IP Address: " IPSTR "\n",
               IP2STR(&event->ip_info.ip));
        break;
      }
      default:
        break;
    }
  }
}

void wifi_init(void) {
  ESP_ERROR_CHECK(esp_netif_init());

  ESP_ERROR_CHECK(esp_event_loop_create_default());

  esp_netif_t* sta_netif = esp_netif_create_default_wifi_sta();

  ESP_ERROR_CHECK(esp_netif_set_hostname(sta_netif, hostname));

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &wifi_event_handler, NULL));

  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &wifi_event_handler, NULL));

  wifi_config_t wifi_config = {
      .sta =
          {
              .ssid = WIFI_SSID,
              .password = WIFI_PASSWORD,
          },
  };

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

  ESP_ERROR_CHECK(esp_wifi_start());

  printf("Wi-Fi initialized\n");
}

#pragma endregion

// RX Buffer
uint8_t rx_buffer[256];

#pragma region GPIO handling
// Gpios
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
#pragma endregion

#pragma region Node and sending (NODE ID HERE)
// Node Information
uint16_t NODE_ID;

void init_node_id(void) {
  NODE_ID = (uint16_t)(((esp_random() % 10000) + 1) & 0xFFFF);
  printf("Node ID: %u\n", NODE_ID);
}
#define COMP_ID 1

// Message structure
typedef enum {
  PACKET_TEXT = 0,
  PACKET_LOCATION = 1,
  PACKET_IMAGE = 2

} PacketType;

typedef struct {
  uint16_t source_id;
  uint32_t packet_id;
  uint8_t ttl;
  uint8_t type;
  char message[48];
} PacketStruct;

#pragma endregion

#pragma region LoRa messages
void LoRa_setup(void) {
  LoRaInit();

  LoRaBegin(915000000, 22, 3.3, false);

  LoRaConfig(7, SX126X_LORA_BW_125_0, SX126X_LORA_CR_4_5, 8, 60, true, false);

  printf("LoRa transceiver node ready\n");
}

void send_message(const char* message, uint16_t ID, uint8_t TTL, int PACKET_ID,
                  PacketType TYPE) {
  PacketStruct packet = {.source_id = ID, .ttl = TTL};

  if (PACKET_ID == 0) {
    packet.packet_id = esp_random();
  } else {
    packet.packet_id = PACKET_ID;
  }

  if (TYPE == NULL) {
    packet.type = PACKET_TEXT;
  } else {
    packet.type = TYPE;
  }

  strncpy(packet.message, message, sizeof(packet.message) - 1);
  packet.message[sizeof(packet.message) - 1] = '\0';

  printf("\n--- Sending Packet ---\n");
  printf("Source : %u\n", packet.source_id);
  printf("Packet : %lu\n", (unsigned long)packet.packet_id);
  printf("TTL    : %u\n", packet.ttl);
  printf("Type   : %u\n", packet.type);
  printf("Text   : %s\n", packet.message);

  bool sent =
      LoRaSend((uint8_t*)&packet, sizeof(PacketStruct), SX126x_TXMODE_SYNC);

  printf(sent ? "Packet sent\n\n" : "Packet failed\n\n");
  printf("Sending %u bytes\n", (unsigned)sizeof(PacketStruct));
}

void handle_receive(void) {
  // Receive message
  uint8_t received = LoRaReceive(rx_buffer, sizeof(rx_buffer));

  // Empty message
  if (received == 0) return;

  // Make sure is packet
  if (received != sizeof(PacketStruct)) {
    printf("Invalid packet size (%u bytes)\n", received);
    return;
  }

  // Load packet
  PacketStruct packet;
  memcpy(&packet, rx_buffer, sizeof(PacketStruct));

  // Print packet
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

    send_message(packet.message, packet.source_id, packet.ttl - 1,
                 packet.packet_id, packet.type);

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

#pragma endregion

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

  if (IS_WIFI_GATEWAY) {
    wifi_init();
  }

  while (1) {
    handle_receive();

    if (gpio_get_level(BUTTON_GPIO) == 0) {
      send_message("SOS", NODE_ID, 5, 0, PACKET_TEXT);
      wait_for_button_release(BUTTON_GPIO);
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}