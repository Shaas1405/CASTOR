#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include "ra01s.h"

#define BUTTON_GPIO 0
#define NODE_ID 2

typedef enum
{
    PACKET_TEXT = 0,
    PACKET_LOCATION = 1,
    PACKET_IMAGE = 2

} PacketType;

typedef struct
{
    uint16_t source_id;
    uint32_t packet_id;
    uint8_t ttl;
    PacketType type;

    char message[48];

} MeshPacket;

uint32_t next_packet_id = 1;

#define PACKET_CACHE_SIZE 100

typedef struct
{
    uint16_t source_id;
    uint32_t packet_id;

} PacketCacheEntry;

PacketCacheEntry packet_cache[PACKET_CACHE_SIZE];
uint16_t cache_index = 0;

bool packet_seen(uint16_t source_id, uint32_t packet_id)
{
    for (int i = 0; i < PACKET_CACHE_SIZE; i++)
    {
        if (packet_cache[i].source_id == source_id &&
            packet_cache[i].packet_id == packet_id)
        {
            return true;
        }
    }

    return false;
}

void cache_packet(uint16_t source_id, uint32_t packet_id)
{
    packet_cache[cache_index].source_id = source_id;
    packet_cache[cache_index].packet_id = packet_id;

    cache_index++;

    if (cache_index >= PACKET_CACHE_SIZE)
        cache_index = 0;
}

void app_main(void)
{
    gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};

    gpio_config(&button_config);

    LoRaInit();

    LoRaBegin(
        915000000,
        22,
        3.3,
        false);

    LoRaConfig(
        7,
        SX126X_LORA_BW_125_0,
        SX126X_LORA_CR_4_5,
        8,
        32,
        true,
        false);

    printf("Mesh node ready\n");

    MeshPacket rx_packet;

    while (1)
    {
        uint8_t received = LoRaReceive(
            (uint8_t *)&rx_packet,
            sizeof(MeshPacket));

        if (received > 0)
        {
            // if (packet_seen(rx_packet.source_id, rx_packet.packet_id))
            // {
            //     printf("Duplicate packet ignored\n");
            //     continue;
            // }
            // cache_packet(rx_packet.source_id, rx_packet.packet_id);

            // printf("New packet!\n");

            // if (rx_packet.ttl > 1)
            // {
            //     rx_packet.ttl--;

            //     LoRaSend(
            //         (uint8_t *)&rx_packet,
            //         sizeof(rx_packet),
            //         SX126x_TXMODE_SYNC);
            // }

            printf("\n----- Packet Received -----\n");
            printf("Source ID : %d\n", rx_packet.source_id);
            printf("Packet ID : %lu\n", (unsigned long)rx_packet.packet_id);
            printf("TTL       : %d\n", rx_packet.ttl);

            switch (rx_packet.type)
            {
            case PACKET_TEXT:
                printf("Type      : TEXT\n");
                break;

            case PACKET_LOCATION:
                printf("Type      : LOCATION\n");
                break;

            case PACKET_IMAGE:
                printf("Type      : IMAGE\n");
                break;
            }

            printf("Message   : %s\n", rx_packet.message);
        }

        if (gpio_get_level(BUTTON_GPIO) == 0)
        {
            MeshPacket tx_packet;

            tx_packet.source_id = NODE_ID;
            tx_packet.packet_id = next_packet_id++;
            tx_packet.ttl = 5;
            tx_packet.type = PACKET_TEXT;

            strcpy(tx_packet.message, "SOS at Building A");

            printf("\nSending packet...\n");

            bool sent = LoRaSend(
                (uint8_t *)&tx_packet,
                sizeof(MeshPacket),
                SX126x_TXMODE_SYNC);

            if (sent)
                printf("Packet sent\n");
            else
                printf("Packet failed\n");

            while (gpio_get_level(BUTTON_GPIO) == 0)
            {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}