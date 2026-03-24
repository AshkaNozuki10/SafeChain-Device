#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

#define LOG_LEVEL_DEFAULT LOG_INFO

// =======================================================
// 1. MASTER HARDWARE SWITCH (Uncomment ONLY ONE)
// =======================================================
#define BOARD_ESP32_C3_SUPER_MINI
// #define BOARD_ESP32_S3_SUPER_MINI

// =======================================================
// 2. DEVICE ROLES (Uncomment ONLY ONE)
// =======================================================
#define ROLE_NODE
// #define ROLE_REPEATER
// #define ROLE_GATEWAY

// =======================================================
// 3. HARDWARE PINS
// =======================================================
#if defined(BOARD_ESP32_C3_SUPER_MINI)
    #define PIN_BUZZER      10 // 2
    #define PIN_GPS_RX      20
    #define PIN_GPS_TX      21
    #define PIN_RGB         9
    #define PIN_BATTERY     0
    #define PIN_BTN_FLOOD   3
    #define PIN_BTN_FIRE    1 
    #define PIN_BTN_CRIME   2 // 10
    #define SCK_PIN         4
    #define MISO_PIN        5
    #define MOSI_PIN        6
    #define SS_PIN          7
    #define RST_PIN         8
    #define DIO0_PIN        -1
#elif defined(BOARD_ESP32_S3_SUPER_MINI)
    #define PIN_BUZZER      9
    #define PIN_GPS_RX      44
    #define PIN_GPS_TX      43
    #define PIN_RGB         48
    #define PIN_BATTERY     3
    #define PIN_BTN_FLOOD   1
    #define PIN_BTN_FIRE    2
    #define PIN_BTN_CRIME   4
    #define SCK_PIN         7
    #define MISO_PIN        5
    #define MOSI_PIN        6
    #define SS_PIN          10
    #define RST_PIN         11
    #define DIO0_PIN        -1
#endif

// =======================================================
// 4. LORA SETTINGS
// =======================================================
#define LORA_FREQ       433E6
#define LORA_SF         11
#define LORA_BW         125E3
#define LORA_CR         8
#define LORA_PREAMBLE   16
#define LORA_SYNCWORD   0xF3
#define LORA_TXPOWER    17

// =======================================================
// 5. PROTOCOL TIMERS
// =======================================================
#define MAX_HOP         10

// [M4] Node no longer relays packets — these settings remain for Repeater only
// Repeater config.h retains its own DUPLICATE_CACHE_SIZE and RELAY delays

#define ACK_TIMEOUT_MS              5000
#define RETRY_INTERVAL_MS           8000
#define MAX_RETRIES                 3
#define FAILED_RETRY_INTERVAL_MS    60000

// =======================================================
// 6. WATCHDOG & RADIO HEALTH  [M1]
// =======================================================
#define WDT_TIMEOUT_S                   10
#define LORA_HEALTH_CHECK_INTERVAL_MS   30000

// =======================================================
// 7. UI TIMERS
// =======================================================
#define AUTO_MODE_INTERVAL      15000
#define LED_UPDATE_INTERVAL     2000
#define BLE_UPDATE_INTERVAL     3000
#define GPS_VALID_AGE_MS        5000

// =======================================================
// 8. HEARTBEAT [M5]
// =======================================================
// How often to broadcast a FRAME_HEARTBEAT while idle
#define HEARTBEAT_INTERVAL_MS       300000UL  // 5 minutes

// Gateway marks a node OFFLINE after this much silence
// Set to 3x interval plus a 3-minute margin for RF variance
#define NODE_OFFLINE_THRESHOLD_MS   1080000UL // 18 minutes

// How often the gateway checks for offline nodes
#define NODE_OFFLINE_CHECK_MS       60000UL   // 1 minute

// =======================================================
// 9. MESSAGE ENUMS
// =======================================================
enum MsgType : uint8_t {
    MSG_EMERGENCY = 0x1A,
    MSG_HEARTBEAT = 0x2A,
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

// =======================================================
// 9. DEFAULT NODE IDs
// =======================================================
#ifdef ROLE_REPEATER
    #define DEFAULT_NODE_ID "REP01"
#elif defined(ROLE_GATEWAY)
    #define DEFAULT_NODE_ID "GTW01"
#else
    #if defined(BOARD_ESP32_S3_SUPER_MINI)
        #define DEFAULT_NODE_ID "S3"
    #else
        #define DEFAULT_NODE_ID "FOB01"
    #endif
#endif

#endif