#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// === HARDWARE PINS (LOCKED) ===
#define PIN_BUZZER      3
#define PIN_GPS_RX      20
#define PIN_GPS_TX      21
#define PIN_RGB         2
#define PIN_BTN_FLOOD   0
#define PIN_BTN_FIRE    1
#define PIN_BTN_CRIME   10

// LoRa SPI
#define SCK_PIN         4
#define MISO_PIN        5
#define MOSI_PIN        6
#define SS_PIN          7
#define RST_PIN         8
#define DIO0_PIN        -1

// === LORA SETTINGS ===
#define LORA_FREQ       433E6
#define LORA_SF         12
#define LORA_BW         125E3
#define LORA_CR         8
#define LORA_PREAMBLE   16
#define LORA_SYNCWORD   0xF3
#define LORA_TXPOWER    20 // Boosted to 20dBm for repeater/gateway

// === PROTOCOL ===
#define MAX_HOP         3
#define DUPLICATE_CACHE_SIZE 20
#define ACK_TIMEOUT_MS  5000
#define RETRY_INTERVAL_MS 8000
#define MAX_RETRIES     3
#define RELAY_MIN_DELAY 500
#define RELAY_MAX_DELAY 1500

// === TIMING ===
#define AUTO_MODE_INTERVAL  15000
#define LED_UPDATE_INTERVAL 2000
#define BLE_UPDATE_INTERVAL 3000
#define GPS_VALID_AGE_MS    5000

// === MESSAGE TYPES ===
enum MsgType : uint8_t {
    MSG_EMERGENCY = 0x01,
    MSG_HEARTBEAT = 0x02,
    MSG_ACK       = 0x03,
    MSG_TEST      = 0xFF
};

enum EmergencyType : uint8_t {
    EM_NONE  = 0x00,
    EM_FIRE  = 0x01,
    EM_FLOOD = 0x02,
    EM_CRIME = 0x03,
    EM_SAFE  = 0x04
};

// === BLE ===
#define SERVICE_UUID        "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_RX        "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_TX        "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// === DEFAULTS ===
#define DEFAULT_NODE_ID     "FOB01"

#endif