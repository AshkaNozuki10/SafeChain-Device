#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// === HARDWARE PINS ===
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
#define LORA_TXPOWER    20

// === PROTOCOL ===
#define DUPLICATE_CACHE_SIZE    100
#define MAX_EVENTS              50
#define DEFAULT_NODE_ID         "GTW01"

// === HEARTBEAT & NODE REGISTRY [M5] ===
#define NODE_OFFLINE_THRESHOLD_MS   1080000UL // 18 minutes
#define NODE_OFFLINE_CHECK_MS       60000UL   // 1 minute

// === WATCHDOG & HEALTH [M1] ===
#define WDT_TIMEOUT_S               15
#define LORA_HEALTH_CHECK_INTERVAL_MS 30000

// === MESSAGE TYPES ===
enum MsgType : uint8_t {
    MSG_EMERGENCY = 0x01A,
    MSG_HEARTBEAT = 0x02A,
    MSG_ACK       = 0x0A,
    MSG_TEST      = 0xFF
};

enum EmergencyType : uint8_t {
    EM_NONE  = 0x00,
    EM_FIRE  = 0x01,
    EM_FLOOD = 0x02,
    EM_CRIME = 0x03,
    EM_SAFE  = 0x04,
    EM_TEST  = 0xFF
};

#endif