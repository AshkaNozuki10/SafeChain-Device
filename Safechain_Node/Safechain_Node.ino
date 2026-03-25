// ============================================================
// Safechain_Node.ino  —  M0 + M1 + M2(NVS journal) + M3(auth)
// ============================================================
#include <SPI.h>
#include <LoRa.h>
#include <OneButton.h>
#include <esp_task_wdt.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>

#include "config.h"

#include "emergency.h"
#include "ble_terminal.h"
#include "gps_manager.h"
#include "led_manager.h"
#include "storage.h"
#include "safechain_protocol.h"
#include "debug_log.h"

// ============================================================
// GLOBAL OBJECTS
// ============================================================
Storage          storage;
EmergencyManager emergency;
BLETerminal      ble;
GPSManager       gpsManager;
LEDManager       led;

OneButton btnFlood(PIN_BTN_FLOOD, true);
OneButton btnFire (PIN_BTN_FIRE,  true);
OneButton btnCrime(PIN_BTN_CRIME, true);

String        nodeID;
bool          autoMode            = false;
unsigned long lastAutoTime        = 0;
unsigned long lastLedUpdate       = 0;
unsigned long lastBleUpdate       = 0;
unsigned long lastBatteryWarnTime = 0;
unsigned long lastActivityTime    = 0;
unsigned long lastHeartbeatTime   = 0; // [M5]
uint32_t      heartbeatIntervalMs = HEARTBEAT_INTERVAL_MS; // [M6] runtime-adjustable

const unsigned long BATTERY_WARN_INTERVAL = 10000;
const unsigned long SLEEP_TIMEOUT         = 180000;

RTC_DATA_ATTR int bootCount       = 0;
RTC_DATA_ATTR int loraReinitCount = 0;

unsigned long lastLoraHealthCheck = 0;
uint32_t      loraFaultCount      = 0;

// [M3] Node PSK cache — loaded from storage on boot
uint8_t nodePSK[sc::PSK_LEN];

// ============================================================
// [M0-2] NON-BLOCKING SIREN
// ============================================================
struct SirenStep { uint16_t freq; uint16_t dur_ms; };

static const SirenStep SIREN_FLOOD[] = {{2000,300},{0,300},{2000,300},{0,300},{2000,300},{0,350}};
static const SirenStep SIREN_FIRE[]  = {{1000,280},{2500,280},{1000,280},{2500,280},{1000,280},{2500,300}};
static const SirenStep SIREN_CRIME[] = {{3500,60},{2000,60},{3500,60},{2000,60},{3500,60},{2000,60},{3500,60},{2000,70}};
static const SirenStep SIREN_SAFE[]  = {{1000,150},{0,80},{2000,200}};

static const SirenStep* activeSirenSteps = nullptr;
static uint8_t        sirenStepCount     = 0;
static uint8_t        sirenStepIdx       = 0;
static unsigned long  sirenStepAt        = 0;

void sirenPlay(const SirenStep* steps, uint8_t count) {
    activeSirenSteps = steps; sirenStepCount = count; sirenStepIdx = 0;
    sirenStepAt = millis();
    if (steps[0].freq > 0) {
        ledcWriteTone(PIN_BUZZER, steps[0].freq);
        ledcWrite(PIN_BUZZER, 128); // 50% volume
    } else {
        ledcWrite(PIN_BUZZER, 0);   // Mute
    }
}

void sirenUpdate() {
    if (!activeSirenSteps) return;
    if ((millis() - sirenStepAt) < activeSirenSteps[sirenStepIdx].dur_ms) return;
    
    if (++sirenStepIdx >= sirenStepCount) { 
        activeSirenSteps = nullptr; 
        ledcWrite(PIN_BUZZER, 0); 
        return; 
    }
    
    sirenStepAt = millis();
    if (activeSirenSteps[sirenStepIdx].freq > 0) {
        ledcWriteTone(PIN_BUZZER, activeSirenSteps[sirenStepIdx].freq);
        ledcWrite(PIN_BUZZER, 128);
    } else {
        ledcWrite(PIN_BUZZER, 0);
    }
}

// ============================================================
// [FIX] Direct SPI register read (LoRa.readRegister() is private)
// ============================================================
uint8_t readLoRaRegister(uint8_t reg) {
    digitalWrite(SS_PIN, LOW);
    SPI.transfer(reg & 0x7F);
    uint8_t val = SPI.transfer(0x00);
    digitalWrite(SS_PIN, HIGH);
    return val;
}

// ============================================================
// FORWARD DECLARATIONS
// ============================================================
void    handleCommand(String cmd);
void    sendTestPacket();
void    triggerFlood();
void    triggerFire();
void    triggerCrime();
void    triggerSafe();
void    onGPSInject(float lat, float lon);
void    onBLECommand(String cmd);
void    flashRGB(int r, int g, int b, int times);
uint8_t getBatteryLevel();
bool    loraReinit();
void    checkLoraHealth();
void    printMenu();
void    sendHeartbeat(); // [M5]
void    handleConfigFrame(const sc::SafeChainFrameV1 &frame); // [M6]
void    loadNodePSK();

// ============================================================
// PSK loader — shared between setup() and setpsk command
// ============================================================
void loadNodePSK() {
    storage.getPSK(nodePSK, sc::PSK_LEN);
    emergency.reloadPSK();
}

// ============================================================
// SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(2000);
    while (!Serial && millis() < 5000) delay(10);

    bootCount++;
    Serial.printf("\n\n=== SAFECHAIN NODE v2.2 (Boot #%d) ===\n", bootCount);
    DebugLog::init(LOG_LEVEL_DEFAULT);

    // [M1-2] Reboot reason
    esp_reset_reason_t reason = esp_reset_reason();
    const char* rs = "unknown";
    switch (reason) {
        case ESP_RST_POWERON:   rs = "power-on";           break;
        case ESP_RST_BROWNOUT:  rs = "brownout";           break;
        case ESP_RST_TASK_WDT:  rs = "TASK-WATCHDOG";      break;
        case ESP_RST_INT_WDT:   rs = "INTERRUPT-WATCHDOG"; break;
        case ESP_RST_PANIC:     rs = "panic";              break;
        case ESP_RST_DEEPSLEEP: rs = "deep-sleep-wakeup";  break;
        case ESP_RST_SW:        rs = "software";           break;
        default: break;
    }
    Serial.printf(">>> Reset reason: %s\n", rs);
    if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT)
        Serial.println("!!! WATCHDOG RESET DETECTED !!!");

    // [M1-1] Watchdog
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WDT_TIMEOUT_S * 1000, .idle_core_mask = 0, .trigger_panic = true
    };
    esp_err_t wdtErr = esp_task_wdt_init(&wdt_config);
    if (wdtErr == ESP_ERR_INVALID_STATE) esp_task_wdt_reconfigure(&wdt_config);
    esp_err_t addErr = esp_task_wdt_add(NULL);
    if (addErr != ESP_OK && addErr != ESP_ERR_INVALID_STATE)
        Serial.printf(">>> WDT add failed: %d\n", (int)addErr);
    esp_task_wdt_reset();

    storage.init();
    nodeID = storage.getNodeID();
    loadNodePSK(); // [M3]

    // [M6] Load runtime-adjustable config from NVS
    LoRa.setTxPower(storage.getTxPower());
    heartbeatIntervalMs = storage.getHBInterval();
    Serial.printf(">>> Node ID: %s | PSK: %s\n",
        nodeID.c_str(), storage.hasPSK() ? "provisioned" : "dev-default");
    Serial.printf("NODE FW=v2.2 | V1_SIZE=%u\n", (unsigned)sizeof(sc::SafeChainFrameV1));

    #if defined(BOARD_ESP32_S3_SUPER_MINI) || defined(BOARD_ESP32_C3_SUPER_MINI)
        ledcAttach(PIN_BUZZER, 2000, 8); // 👇 FIX: Attach hardware PWM channel!
        ledcWrite(PIN_BUZZER, 0);
    #else
        pinMode(PIN_BUZZER, OUTPUT);
        digitalWrite(PIN_BUZZER, LOW);
    #endif

    btnFlood.setDebounceMs(80); btnFlood.setPressMs(1200);
    btnFire .setDebounceMs(80); btnFire .setPressMs(1200);
    btnCrime.setDebounceMs(80); btnCrime.setPressMs(1200);
    btnFlood.attachLongPressStart(triggerFlood);
    btnFire .attachLongPressStart(triggerFire);
    btnCrime.attachLongPressStart(triggerCrime);
    btnFlood.attachDoubleClick(triggerSafe);
    btnFire .attachDoubleClick(triggerSafe);
    btnCrime.attachDoubleClick(triggerSafe);

    gpsManager.init();
    esp_task_wdt_reset();

    ble.init(nodeID.c_str());
    ble.setGPSInjectCallback(onGPSInject);
    ble.setCommandCallback(onBLECommand);
    esp_task_wdt_reset();


    emergency.init(nodeID.c_str(), &storage); // [M3] loads PSK inside init()

    SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
    LoRa.setPins(SS_PIN, RST_PIN, DIO0_PIN);
    if (!LoRa.begin(LORA_FREQ)) {
        Serial.println("LoRa Init Failed!");
        led.setColor(255, 0, 0);
        while (1) { esp_task_wdt_reset(); delay(1000); }
    }
    LoRa.setSpreadingFactor(storage.getSpreadingFactor());
    LoRa.setSignalBandwidth(LORA_BW);
    LoRa.setCodingRate4(LORA_CR);
    LoRa.setPreambleLength(LORA_PREAMBLE);
    LoRa.enableCrc();
    LoRa.setSyncWord(LORA_SYNCWORD);
    LoRa.setTxPower(LORA_TXPOWER);
    randomSeed(esp_random());
    LoRa.receive();
    esp_task_wdt_reset();

    // [M0-4] Resume before wakeup
    bool resumed = emergency.resumePending();
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    uint64_t wakeup_pin_mask = 0;
    #if defined(BOARD_ESP32_S3_SUPER_MINI)
        if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1)
            wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();
    #elif defined(BOARD_ESP32_C3_SUPER_MINI)
        if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO)
            wakeup_pin_mask = esp_sleep_get_gpio_wakeup_status();
    #endif

    if (!resumed && wakeup_pin_mask != 0) {
        Serial.println(">>> WOKE FROM DEEP SLEEP VIA BUTTON");
        // [FIX Bug2] Do NOT auto-trigger emergency on wakeup button.
        // Wakeup bypassed the 1200ms long-press guard, causing accidental
        // alerts on every sleep→wake cycle. User must long-press after waking.
        lastActivityTime = millis(); // prevent immediate re-sleep
    } else if (resumed) {
        Serial.println(">>> Skipped wakeup — NVS pending event has priority");
    } else {
        Serial.println(">>> Normal Boot / Power On");
    }

    lastActivityTime    = millis();
    lastLoraHealthCheck = millis();
    esp_task_wdt_reset();

    Serial.println("LoRa Ready (SF11 + CRC + AUTH)");
    printMenu();
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
    esp_task_wdt_reset();

    btnFlood.tick(); btnFire.tick(); btnCrime.tick();
    sirenUpdate();
    gpsManager.update();
    ble.update();
    int packetSize = LoRa.parsePacket();

    if (packetSize == sizeof(sc::SafeChainFrameV1)) {
        sc::SafeChainFrameV1 rxFrame;
        LoRa.readBytes((uint8_t*)&rxFrame, sizeof(sc::SafeChainFrameV1));

        // [M3] validateFrame now checks both CRC16 and HMAC
        if (sc::Protocol::validateFrame(rxFrame, nodePSK, sc::PSK_LEN)) {
            rxFrame.last_rssi_dbm = LoRa.packetRssi();
            Serial.printf("\n[RX V1] Frame=%s Event=%s EventID=%lu Hop=%u RSSI=%d auth=OK\n",
                sc::Protocol::frameTypeName(rxFrame.frame_type),
                sc::Protocol::eventTypeName(rxFrame.event_type),
                (unsigned long)rxFrame.event_id, rxFrame.hop_count, rxFrame.last_rssi_dbm);
            led.flash(0, 255, 255, 1);
            if (rxFrame.frame_type == sc::FRAME_ACK)
                emergency.handleACK(rxFrame);
            else if (rxFrame.frame_type == sc::FRAME_CONFIG)
                handleConfigFrame(rxFrame); // [M6]
        } else {
            Serial.println(">>> VALIDATE FAIL - CRC or AUTH mismatch — Dropped");
        }

    } else if (packetSize > 0) {
        while (LoRa.available()) LoRa.read();
        Serial.printf(">>> WARNING: Unknown packet size: %d\n", packetSize);
    }

    // [FIX Bug1] emergency.update() runs AFTER RX so incoming ACKs are
    // processed before ACK_TIMEOUT can advance the state to FAILED.
    // Previously: update() ran first → state changed → handleACK rejected
    // the valid ACK because state was no longer WAITING_ACK.
    emergency.update();

    checkLoraHealth();

    if ((millis() - lastLedUpdate) > LED_UPDATE_INTERVAL) {
        lastLedUpdate = millis();
        EmergencyState emState = emergency.getState();
        if      (emState == EM_STATE_WAITING_ACK) led.flash(255, 165, 0, 1);
        else if (emState == EM_STATE_FAILED)       led.flash(255, 0,   0, 1);
        else if (emState == EM_STATE_CONFIRMED)    led.flash(0,   255, 0, 1);
        else                                       led.updateStatus(ble.isConnected(), gpsManager.isGPSValid());
    }

    if ((millis() - lastBatteryWarnTime) > BATTERY_WARN_INTERVAL) {
        lastBatteryWarnTime = millis();
        if (getBatteryLevel() <= 15) { Serial.println("WARNING: Low Battery!"); led.flash(255, 0, 0, 2); }
    }

    if (ble.isConnected() && (millis() - lastBleUpdate) > BLE_UPDATE_INTERVAL) {
        lastBleUpdate = millis();
        float lat, lon;
        gpsManager.determineLocation(lat, lon, false);
        ble.send(String(lat, 6) + "," + String(lon, 6));
    }

    if (autoMode && (millis() - lastAutoTime) > AUTO_MODE_INTERVAL) {
        lastAutoTime = millis();
        sendTestPacket();
    }

    // [M5] Periodic heartbeat — only while idle, avoids colliding with emergency TX
    if (emergency.getState() == EM_STATE_IDLE &&
        (millis() - lastHeartbeatTime) >= heartbeatIntervalMs) { // [M6] runtime interval
        lastHeartbeatTime = millis();
        sendHeartbeat();
    }

    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        handleCommand(cmd);
    }

    delay(10);

    if (emergency.getState() == EM_STATE_IDLE) {
        if (btnFlood.isLongPressed() || btnFire.isLongPressed() || btnCrime.isLongPressed())
            lastActivityTime = millis();
        if ((millis() - lastActivityTime) > SLEEP_TIMEOUT) {
            Serial.println("\nEntering Deep Sleep...");
            ble.send("Node entering Deep Sleep.");
            delay(400); led.off(); LoRa.sleep();
            uint64_t wakeMask = (1ULL << PIN_BTN_FLOOD) | (1ULL << PIN_BTN_FIRE);
            #if defined(BOARD_ESP32_S3_SUPER_MINI)
                rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_FLOOD);
                rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_FIRE);
                rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_CRIME);
                rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN_FLOOD);
                rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN_FIRE);
                rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN_CRIME);
                esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
            #elif defined(BOARD_ESP32_C3_SUPER_MINI)
                pinMode(PIN_BTN_FLOOD, INPUT_PULLUP);
                pinMode(PIN_BTN_FIRE,  INPUT_PULLUP);
                pinMode(PIN_BTN_CRIME, INPUT_PULLUP);
                gpio_hold_en((gpio_num_t)PIN_BTN_FLOOD);
                gpio_hold_en((gpio_num_t)PIN_BTN_FIRE);
                gpio_hold_en((gpio_num_t)PIN_BTN_CRIME);
                gpio_deep_sleep_hold_en();
                esp_deep_sleep_enable_gpio_wakeup(wakeMask, ESP_GPIO_WAKEUP_GPIO_LOW);
            #endif
            esp_deep_sleep_start();
        }
    } else {
        lastActivityTime = millis();
    }
}

// ============================================================
// LoRa reinit [M1-3]
// ============================================================
bool loraReinit() {
    Serial.printf(">>> LoRa reinit (fault #%lu)...\n", (unsigned long)(loraFaultCount + 1));
    LoRa.end(); delay(150);
    pinMode(RST_PIN, OUTPUT);
    digitalWrite(RST_PIN, HIGH); delay(10);
    digitalWrite(RST_PIN, LOW);  delay(10);
    digitalWrite(RST_PIN, HIGH); delay(20);
    bool ok = false;
    for (int i = 1; i <= 3; i++) { if (LoRa.begin(LORA_FREQ)) { ok = true; break; } delay(200); }
    if (!ok) { loraFaultCount++; loraReinitCount++; return false; }
    LoRa.setSpreadingFactor(storage.getSpreadingFactor());
    LoRa.setSignalBandwidth(LORA_BW); LoRa.setCodingRate4(LORA_CR);
    LoRa.setPreambleLength(LORA_PREAMBLE); LoRa.enableCrc();
    LoRa.setSyncWord(LORA_SYNCWORD); LoRa.setTxPower(LORA_TXPOWER); LoRa.receive();
    loraReinitCount++;
    Serial.printf(">>> LoRa reinit OK (#%d)\n", loraReinitCount);
    return true;
}

void checkLoraHealth() {
    if ((millis() - lastLoraHealthCheck) < LORA_HEALTH_CHECK_INTERVAL_MS) return;
    if (emergency.getState() == EM_STATE_PENDING_TX) return;
    lastLoraHealthCheck = millis();
    LoRa.receive();
    uint8_t v = readLoRaRegister(0x42);
    if (v != 0x12) { Serial.printf(">>> LoRa health FAIL: 0x%02X\n", v); loraFaultCount++; loraReinit(); }
    else Serial.printf(">>> LoRa health OK | faults=%lu\n", (unsigned long)loraFaultCount);
}

// ============================================================
// HELPERS
// ============================================================
void flashRGB(int r, int g, int b, int times) {
    led.flash(r, g, b, times);
    led.updateStatus(ble.isConnected(), gpsManager.isGPSValid());
}

uint8_t getBatteryLevel() {
    uint32_t total = 0;
    for (int i = 0; i < 10; i++) { total += analogReadMilliVolts(PIN_BATTERY); delay(2); }
    float v = (total / 10.0f / 1000.0f) * 2.0f;
    if (v >= 4.20f) return 100;
    if (v <= 3.20f) return 0;
    return (uint8_t)(v >= 3.80f
        ? 50.0f + ((v - 3.80f) / 0.40f) * 50.0f
        : ((v - 3.20f) / 0.60f) * 50.0f);
}

void sendTestPacket() {
    float lat, lon;
    gpsManager.determineLocation(lat, lon, true);
    bool gpsValid   = !(lat == 0.0f && lon == 0.0f);
    bool lowBattery = getBatteryLevel() <= 15;
    sc::SafeChainFrameV1 frame;
    sc::Protocol::buildEventFrame(
        frame, nodeID.c_str(), nodeID.c_str(),
        0,                          // event_id 0 = test, not stored in journal
        sc::EVENT_TEST,
        0, millis(),
        (int32_t)(lat * 10000000.0f),
        (int32_t)(lon * 10000000.0f),
        getBatteryLevel(), gpsValid, lowBattery,
        nodePSK, sc::PSK_LEN
    );
    Serial.println(">>> SENDING V1 TEST...");
    ble.send("SENDING V1 TEST PING...");
    led.flash(255, 255, 255, 2); 
    ledcWriteTone(PIN_BUZZER, 3000);
    ledcWrite(PIN_BUZZER, 128);
    delay(100);
    ledcWrite(PIN_BUZZER, 0);
    LoRa.idle(); LoRa.beginPacket();
    LoRa.write((uint8_t*)&frame, sizeof(sc::SafeChainFrameV1));
    LoRa.endPacket(); LoRa.receive();
    Serial.println(">>> SENT!"); ble.send("V1 TEST PACKET AIRBORNE!");
}

void triggerFlood() {
    led.setColor(0, 0, 255); ble.send("FLOOD");
    float lat, lon; gpsManager.determineLocation(lat, lon, true);
    emergency.trigger(EM_FLOOD, lat, lon, getBatteryLevel());
    sirenPlay(SIREN_FLOOD, sizeof(SIREN_FLOOD)/sizeof(SIREN_FLOOD[0]));
}

void triggerFire() {
    led.setColor(255, 50, 0); ble.send("FIRE");
    float lat, lon; gpsManager.determineLocation(lat, lon, true);
    emergency.trigger(EM_FIRE, lat, lon, getBatteryLevel());
    sirenPlay(SIREN_FIRE, sizeof(SIREN_FIRE)/sizeof(SIREN_FIRE[0]));
}

void triggerCrime() {
    led.setColor(255, 0, 0); ble.send("CRIME");
    float lat, lon; gpsManager.determineLocation(lat, lon, true);
    emergency.trigger(EM_CRIME, lat, lon, getBatteryLevel());
    sirenPlay(SIREN_CRIME, sizeof(SIREN_CRIME)/sizeof(SIREN_CRIME[0]));
}

void triggerSafe() {
    if (emergency.getState() == EM_STATE_IDLE) { Serial.println(">>> Already idle."); return; }
    led.setColor(0, 255, 0); ble.send("MARKED SAFE");
    float lat, lon; gpsManager.determineLocation(lat, lon, true);
    emergency.trigger(EM_SAFE, lat, lon, getBatteryLevel());
    sirenPlay(SIREN_SAFE, sizeof(SIREN_SAFE)/sizeof(SIREN_SAFE[0]));
}

void onGPSInject(float lat, float lon) { gpsManager.injectBLE(lat, lon); }
void onBLECommand(String cmd) { handleCommand(cmd); }

void handleCommand(String input) {
    input.trim(); input.toLowerCase();
    int spaceIdx = input.indexOf(' ');
    String cmd = (spaceIdx == -1) ? input : input.substring(0, spaceIdx);
    String arg = (spaceIdx == -1) ? "" : input.substring(spaceIdx + 1);
    String response = "";

    if      (cmd == "flood")  { triggerFlood(); }
    else if (cmd == "fire")   { triggerFire(); }
    else if (cmd == "crime")  { triggerCrime(); }
    else if (cmd == "safe")   { triggerSafe(); }
    else if (cmd == "send")   { sendTestPacket(); }
    else if (cmd == "gps") {
        float lat, lon; gpsManager.determineLocation(lat, lon, true);
        response = "GPS: " + String(lat,6) + ", " + String(lon,6) + " (" + gpsManager.getSourceName() + ")";
    }
    else if (cmd == "relay") {
        // [M4] Node no longer relays legacy packets — relay setting preserved for future V1 relay support
        response = "Relay: disabled in M4 (V1-only mode)";
    }
    else if (cmd == "uid") {
        if (arg.length() > 0) { nodeID = arg.substring(0,5); storage.setNodeID(nodeID); response = "UID saved: " + nodeID + " (reboot)"; }
        else                   { response = "Current UID: " + nodeID; }
    }
    else if (cmd == "sf") {
        if (arg.length() > 0) { uint8_t sf = arg.toInt(); if (sf>=7&&sf<=12) { storage.setSpreadingFactor(sf); LoRa.setSpreadingFactor(sf); response = "SF=" + String(sf); } }
        else                   { response = "SF=" + String(storage.getSpreadingFactor()); }
    }
    else if (cmd == "auto") {
        if      (arg == "on")  { autoMode = true;  response = "Auto: ON"; }
        else if (arg == "off") { autoMode = false; response = "Auto: OFF"; }
        else                   { response = "Auto: " + String(autoMode ? "ON" : "OFF"); }
    }
    // --------------------------------------------------------
    // [M3] setpsk <32 hex chars> — provision a 16-byte PSK
    // Example: setpsk 0102030405060708090a0b0c0d0e0f10
    // --------------------------------------------------------
    else if (cmd == "setpsk") {
        if (arg.length() == 32) {
            uint8_t newKey[sc::PSK_LEN];
            bool ok = true;
            for (int i = 0; i < sc::PSK_LEN; i++) {
                String byteStr = arg.substring(i * 2, i * 2 + 2);
                long val = strtol(byteStr.c_str(), nullptr, 16);
                if (val < 0 || val > 255) { ok = false; break; }
                newKey[i] = (uint8_t)val;
            }
            if (ok) {
                storage.setPSK(newKey, sc::PSK_LEN);
                loadNodePSK();
                response = "PSK provisioned. Reboot to apply fully.";
            } else {
                response = "Invalid hex — use 32 hex chars (e.g. 0102...0f10)";
            }
        } else {
            response = "Usage: setpsk <32 hex chars>";
        }
    }
    // [M3] showpsk — display current PSK status and first 4 bytes
    else if (cmd == "showpsk") {
        char buf[9];
        snprintf(buf, sizeof(buf), "%02X%02X%02X%02X", nodePSK[0], nodePSK[1], nodePSK[2], nodePSK[3]);
        response = "PSK: " + String(storage.hasPSK() ? "provisioned" : "dev-default")
                 + " | first4=" + String(buf);
    }
    else if (cmd == "info") {
        response  = "=== NODE INFO ===\n";
        response += "UID: "         + nodeID + "\n";
        response += "MAC: "         + ble.getMacAddress() + "\n";
        response += "SF: "          + String(storage.getSpreadingFactor()) + "\n";
        response += "BLE: "         + String(ble.isConnected() ? "Connected" : "Disconnected") + "\n";
        response += "GPS: "         + String(gpsManager.isGPSValid() ? "Valid" : "No Fix") + "\n";
        response += "Battery: "     + String(getBatteryLevel()) + "%\n";
        response += "PSK: "         + String(storage.hasPSK() ? "provisioned" : "dev-default") + "\n";
        response += "LoRa faults: " + String(loraFaultCount) + " reinits: " + String(loraReinitCount) + "\n";
        response += "Boot count: "  + String(bootCount);
    }
    else if (cmd == "reboot") {
        response = "Rebooting..."; Serial.println(response); ble.send(response); delay(500); ESP.restart();
    }
    else { response = "Unknown: " + cmd; printMenu(); }

    if (response != "") { Serial.println(response); ble.send(response); }
}

// ============================================================
// [M5] HEARTBEAT — non-emergency liveness broadcast
// Carries battery level and GPS location. No ACK requested.
// Gateway uses this to track node online/offline status.
// ============================================================
void sendHeartbeat() {
    float lat, lon;
    gpsManager.determineLocation(lat, lon, false);
    bool gpsValid   = !(lat == 0.0f && lon == 0.0f);
    uint8_t batt    = getBatteryLevel();
    bool lowBattery = batt <= 15;

    sc::SafeChainFrameV1 frame;
    sc::Protocol::initFrame(frame);
    frame.frame_type = sc::FRAME_HEARTBEAT;
    frame.event_type = sc::EVENT_NONE;
    frame.flags      = 0; // no ACK needed
    if (gpsValid)   frame.flags |= sc::FLAG_GPS_VALID;
    if (lowBattery) frame.flags |= sc::FLAG_LOW_BATTERY;

    strncpy(frame.origin_id, nodeID.c_str(), sc::DEVICE_ID_LEN - 1);
    strncpy(frame.sender_id, nodeID.c_str(), sc::DEVICE_ID_LEN - 1);
    frame.event_id      = 0; // heartbeat has no event_id
    frame.attempt       = 0;
    frame.hop_count     = 0;
    frame.max_hops      = sc::MAX_HOPS_DEFAULT;
    frame.event_time_ms = millis();
    frame.lat_e7        = (int32_t)(lat * 10000000.0f);
    frame.lon_e7        = (int32_t)(lon * 10000000.0f);
    frame.battery_pct   = batt;
    frame.last_rssi_dbm = 0;

    sc::Protocol::finalizeFrame(frame, nodePSK, sc::PSK_LEN);

    Serial.printf(">>> HEARTBEAT: batt=%u%% gps=%s\n",
        batt, gpsValid ? "valid" : "none");

    LoRa.beginPacket();
    LoRa.write((uint8_t*)&frame, sizeof(sc::SafeChainFrameV1));
    LoRa.endPacket();
    LoRa.receive();
}

// ============================================================
// [M6] FRAME_CONFIG handler — applies OTA configuration
// Validates target (nodeID or "ALL"), deduplicates by event_id,
// applies live, persists to NVS so config survives reboot.
// ============================================================
void handleConfigFrame(const sc::SafeChainFrameV1 &frame) {
    // Check if this config targets us or all nodes
    bool targeted = (strncmp(frame.origin_id, nodeID.c_str(), sc::DEVICE_ID_LEN) == 0);
    bool broadcast = (strncmp(frame.origin_id, sc::CONFIG_TARGET_ALL, sc::DEVICE_ID_LEN) == 0);
    if (!targeted && !broadcast) return; // not for us

    // Dedup: ignore if we already applied this config_id
    if (frame.event_id != 0 && frame.event_id == storage.getLastConfigId()) {
        Serial.printf(">>> CONFIG: duplicate event_id %lu — ignored\n",
            (unsigned long)frame.event_id);
        return;
    }

    uint32_t value = frame.event_time_ms; // config_value packed here
    sc::ConfigKey key = static_cast<sc::ConfigKey>(frame.event_type);

    Serial.printf(">>> CONFIG RX: key=0x%02X value=%lu target=%s\n",
        (uint8_t)key, (unsigned long)value,
        broadcast ? "ALL" : nodeID.c_str());
    ble.send("OTA CONFIG RECEIVED");

    switch (key) {
        case sc::CONFIG_SF: {
            uint8_t sf = (uint8_t)value;
            if (sf < 7 || sf > 12) {
                Serial.printf(">>> CONFIG: SF %u out of range (7-12) — rejected\n", sf);
                ble.send("CONFIG REJECTED: SF out of range");
                return;
            }
            storage.setSpreadingFactor(sf);
            LoRa.setSpreadingFactor(sf);
            Serial.printf(">>> CONFIG APPLIED: SF=%u\n", sf);
            ble.send("CONFIG: SF=" + String(sf));
            break;
        }
        case sc::CONFIG_TX_POWER: {
            uint8_t dbm = (uint8_t)value;
            if (dbm < 2 || dbm > 20) {
                Serial.printf(">>> CONFIG: TX power %u out of range (2-20) — rejected\n", dbm);
                ble.send("CONFIG REJECTED: TX power out of range");
                return;
            }
            storage.setTxPower(dbm);
            LoRa.setTxPower(dbm);
            Serial.printf(">>> CONFIG APPLIED: TXpower=%udBm\n", dbm);
            ble.send("CONFIG: TXpower=" + String(dbm) + "dBm");
            break;
        }
        case sc::CONFIG_HB_INTERVAL: {
            if (value < 30000 || value > 1800000) {
                Serial.printf(">>> CONFIG: HB interval %lu out of range (30s-30min) — rejected\n",
                    (unsigned long)value);
                ble.send("CONFIG REJECTED: HB interval out of range");
                return;
            }
            storage.setHBInterval(value);
            heartbeatIntervalMs = value;
            lastHeartbeatTime   = millis(); // reset timer to avoid immediate fire
            Serial.printf(">>> CONFIG APPLIED: HB interval=%lums\n", (unsigned long)value);
            ble.send("CONFIG: HB=" + String(value / 1000) + "s");
            break;
        }
        case sc::CONFIG_REBOOT:
            Serial.println(">>> CONFIG: REBOOT command received");
            ble.send("CONFIG: REBOOTING...");
            storage.setLastConfigId(frame.event_id);
            delay(400);
            ESP.restart();
            return; // never reached
        default:
            Serial.printf(">>> CONFIG: unknown key 0x%02X — ignored\n", (uint8_t)key);
            return;
    }

    // Persist config_id so we don't re-apply on relay bounce
    if (frame.event_id != 0) storage.setLastConfigId(frame.event_id);
}

void printMenu() {
    Serial.println("\n--- COMMANDS ---");
    Serial.println("flood | fire | crime | safe | send");
    Serial.println("gps | relay on/off | uid <id> | sf <7-12>");
    Serial.println("auto on/off | info | reboot");
    Serial.println("setpsk <32hex> | showpsk");
}