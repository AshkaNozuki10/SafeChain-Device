#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// === DEVICE ROLES (Uncomment ONLY one) ===
#define ROLE_NODE
// #define ROLE_REPEATER
// #define ROLE_GATEWAY

// === HARDWARE PINS ===
#define PIN_BUZZER      2
#define PIN_GPS_RX      20
#define PIN_GPS_TX      21
#define PIN_RGB         9 // change back to pin 0 for esp32 s3
// Buttons (Used on Node, optional on Repeater for test)
#define PIN_BTN_FLOOD   3
#define PIN_BTN_FIRE    1
#define PIN_BTN_CRIME   10

#define PIN_BATTERY     0   // Safe ADC1 Pin for voltage reading

// LoRa SPI
#define SCK_PIN         4
#define MISO_PIN        5
#define MOSI_PIN        6
#define SS_PIN          7
#define RST_PIN         8
#define DIO0_PIN        -1

// === LORA SETTINGS ===
#define LORA_FREQ       433E6
#define LORA_SF         11
#define LORA_BW         125E3
#define LORA_CR         8
#define LORA_PREAMBLE   16
#define LORA_SYNCWORD   0xF3
#define LORA_TXPOWER    17  // Boosted to 20dBm for Repeater/Gateway

// === PROTOCOL ===
#define MAX_HOP         10

// Adjust cache based on role
#ifdef ROLE_REPEATER
    #define DUPLICATE_CACHE_SIZE 60 // Remember more packets
    #define RELAY_MIN_DELAY 200     // Faster relay
    #define RELAY_MAX_DELAY 800
#else
    #define DUPLICATE_CACHE_SIZE 20
    #define RELAY_MIN_DELAY 500
    #define RELAY_MAX_DELAY 1500
#endif

#define ACK_TIMEOUT_MS  5000
#define RETRY_INTERVAL_MS 8000
#define MAX_RETRIES     3

// === TIMING ===
#define AUTO_MODE_INTERVAL  15000
#define LED_UPDATE_INTERVAL 2000
#define BLE_UPDATE_INTERVAL 3000  // ← ADD THIS LINE
#define GPS_VALID_AGE_MS    5000  // ← ADD THIS LINE

// === MESSAGE TYPES ===
enum MsgType : uint8_t {
    MSG_EMERGENCY = 0x01,
    MSG_HEARTBEAT = 0x02,
    MSG_ACK       = 0x0A,
    MSG_TEST      = 0xFF
};

enum EmergencyType : uint8_t {
    EM_NONE  = 0x00,
    EM_FIRE  = 0x01,
    EM_FLOOD = 0x02,
    EM_CRIME = 0x03,
    EM_SAFE  = 0x04
};

// === DEFAULTS ===
#ifdef ROLE_REPEATER
    #define DEFAULT_NODE_ID "REP01"
#elif defined(ROLE_GATEWAY)
    #define DEFAULT_NODE_ID "GTW01"
#else
    #define DEFAULT_NODE_ID "FOB01"
#endif

#endif