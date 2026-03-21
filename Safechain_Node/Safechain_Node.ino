//node code

#include <SPI.h>
#include <LoRa.h>
#include <OneButton.h>
#include <driver/rtc_io.h> // 👇 ADD THIS LINE

#include "config.h"
#include "packet.h"
#include "router.h"
#include "emergency.h"
#include "ble_terminal.h"
#include "gps_manager.h"
#include "led_manager.h"
#include "storage.h"

// === GLOBAL OBJECTS ===
Storage storage;
Router router;
EmergencyManager emergency;
BLETerminal ble;
GPSManager gpsManager;
LEDManager led;

// Buttons
OneButton btnFlood(PIN_BTN_FLOOD, true);
OneButton btnFire(PIN_BTN_FIRE, true);
OneButton btnCrime(PIN_BTN_CRIME, true);

// State
String nodeID;
bool autoMode = false;
unsigned long lastAutoTime = 0;
unsigned long lastLedUpdate = 0;
unsigned long lastBleUpdate = 0;

unsigned long lastBatteryWarnTime = 0;
const unsigned long BATTERY_WARN_INTERVAL = 10000; // 10 seconds

// 👇 ADD THESE TWO LINES RIGHT HERE!
bool needsGPSRefine = false;
EmergencyType activeEmergency = EM_NONE;

// --- POWER MANAGEMENT ---
unsigned long lastActivityTime = 0;
const unsigned long SLEEP_TIMEOUT = 180000; // 3 Minutes (180,000 ms)
RTC_DATA_ATTR int bootCount = 0; // Remembers how many times it woke up from sleep

// === FORWARD DECLARATIONS ===
void handleCommand(String cmd);
void sendTestPacket();
void triggerFlood();
void triggerFire();
void triggerCrime();
void triggerSafe();
void onGPSInject(float lat, float lon);
void onBLECommand(String cmd);
void flashRGB(int r, int g, int b, int times);
uint8_t getBatteryLevel();

// === SETUP ===
void setup() {
     Serial.begin(115200);
    
    // Wait for Serial Monitor to connect (ESP32-C3 USB CDC fix)
    delay(2000);  // ← ADD THIS
    while (!Serial && millis() < 5000) {
        delay(10);  // Wait up to 5 seconds for Serial Monitor
    }
    
    Serial.println("\n\n=== SAFECHAIN LITE v2.0 ===");
    
    // Storage
    storage.init();
    nodeID = storage.getNodeID();
    Serial.printf(">>> Node ID: %s\n", nodeID.c_str());
    
    // Buzzer
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_BUZZER, LOW);
    
    // LED
    led.init();
    led.flash(50, 50, 50, 1);
    
   // === Buttons ===
    
    // 1. Increase Debounce Time to 80ms to forgive jittery/bouncing metal contacts
    btnFlood.setDebounceMs(80);
    btnFire.setDebounceMs(80);
    btnCrime.setDebounceMs(80);

    // 2. Set the Hold duration to 1.5 seconds (1500ms) for better emergency UX
    btnFlood.setPressMs(1200);
    btnFire.setPressMs(1200);
    btnCrime.setPressMs(1200);

    // 3. Attach LONG PRESS to trigger the actual emergencies
    btnFlood.attachLongPressStart(triggerFlood);
    btnFire.attachLongPressStart(triggerFire);
    btnCrime.attachLongPressStart(triggerCrime);
    
    // 4. Attach DOUBLE CLICK to the new "Safe" function
    btnFlood.attachDoubleClick(triggerSafe);
    btnFire.attachDoubleClick(triggerSafe);
    btnCrime.attachDoubleClick(triggerSafe);
    
    // GPS
    gpsManager.init();
    
    // BLE
    ble.init(nodeID.c_str());
    ble.setGPSInjectCallback(onGPSInject);
    ble.setCommandCallback(onBLECommand);
    
    // Router
    router.init(nodeID.c_str(), storage.getRelayEnabled());
    
    // Emergency
    emergency.init(nodeID.c_str());
    
    // LoRa
    SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
    LoRa.setPins(SS_PIN, RST_PIN, DIO0_PIN);
    
    if (!LoRa.begin(LORA_FREQ)) {
        Serial.println("❌ LoRa Init Failed!");
        led.setColor(255, 0, 0);
        while(1);
    }
    
    LoRa.setSpreadingFactor(storage.getSpreadingFactor());
    LoRa.setSignalBandwidth(LORA_BW);
    LoRa.setCodingRate4(LORA_CR);
    LoRa.setPreambleLength(LORA_PREAMBLE);
    LoRa.enableCrc();
    LoRa.setSyncWord(LORA_SYNCWORD);
    LoRa.setTxPower(LORA_TXPOWER);
    
    randomSeed(analogRead(0));
    LoRa.sleep();

    // To this:
    LoRa.receive();
 
    
    
    // === DEEP SLEEP WAKE-UP HANDLER ===
    // 1. Ask the ESP32 why it just booted up
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    
    // 👇 FIX: Check for EXT1 wakeup
    if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
        Serial.println("\n⚠️ WOKE UP FROM DEEP SLEEP VIA BUTTON!");
        
        // 👇 FIX: Ask the EXT1 controller exactly WHICH pin triggered the wake-up
        uint64_t wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();

        // 4. Instantly trigger the correct emergency!
        if (wakeup_pin_mask & (1ULL << PIN_BTN_FLOOD)) {
            Serial.println(">>> WAKEUP REASON: FLOOD BUTTON");
            triggerFlood();
            
        } else if (wakeup_pin_mask & (1ULL << PIN_BTN_FIRE)) {
            Serial.println(">>> WAKEUP REASON: FIRE BUTTON");
            triggerFire();
            
        } else if (wakeup_pin_mask & (1ULL << PIN_BTN_CRIME)) {
            Serial.println(">>> WAKEUP REASON: CRIME BUTTON");
            triggerCrime();
        }
        
    } else {
        // Normal power-on or manual reset button
        Serial.println("\n>>> Normal Boot / Power On");
    }

    Serial.println("✅ LoRa Ready (SF12 + CRC + SYNC)");
    Serial.println("\n--- Ready for Commands ---");
    printMenu();
}

// === MAIN LOOP ===
void loop() {
    // Button ticks
    btnFlood.tick();
    btnFire.tick();
    btnCrime.tick();
    
    // GPS update
    gpsManager.update();
    
    // BLE update
    ble.update();
    
    // Emergency state machine
    emergency.update();
    
    // Router relay processing
    router.update();
    
    // LoRa RX
    int packetSize = LoRa.parsePacket();
    if (packetSize == sizeof(SafeChainPacket)) {
        SafeChainPacket rxPkt;
        LoRa.readBytes((uint8_t*)&rxPkt, sizeof(SafeChainPacket));
        
        if (PacketBuilder::validate(rxPkt)) {
            rxPkt.rssi = LoRa.packetRssi();  // Store RSSI
            
            Serial.printf("\n[LORA RX] ");
            PacketBuilder::print(rxPkt);
            Serial.printf("RSSI: %d dBm\n", rxPkt.rssi);
            
            led.flash(0, 255, 255, 2);  // Cyan
            
            // Handle ACK
            if (rxPkt.msgType == MSG_ACK) {
                emergency.handleACK(rxPkt);
            }
            
            // Queue relay if applicable
            if (router.shouldRelay(rxPkt)) {
                led.flash(255, 0, 255, 1);  // Magenta
                router.queueRelay(rxPkt);
            }
        } else {
            Serial.println(">>> CRC FAIL - Dropped");
        }
    }
    
    // LED status update
    if (millis() - lastLedUpdate > LED_UPDATE_INTERVAL) {
        lastLedUpdate = millis();
        led.updateStatus(ble.isConnected(), gpsManager.isGPSValid());
    }

    // 👇 ADD THIS ENTIRE BLOCK: Low Battery Warning
    if (millis() - lastBatteryWarnTime > BATTERY_WARN_INTERVAL) {
        lastBatteryWarnTime = millis();
        
        // Check if the battery is 15% or lower
        if (getBatteryLevel() <= 15) {
            Serial.println("⚠️ WARNING: Low Battery!");
            
            // Flash Red twice quickly to grab the user's attention
            led.flash(255, 0, 0, 2); 
            
            // Restore the normal Bluetooth/GPS dim color after flashing
            led.updateStatus(ble.isConnected(), gpsManager.isGPSValid());
        }
    }
    
    // BLE periodic update
    if (ble.isConnected() && millis() - lastBleUpdate > BLE_UPDATE_INTERVAL) {
        lastBleUpdate = millis();
        float lat, lon;
        gpsManager.determineLocation(lat, lon, false);
        String update = String(lat, 6) + "," + String(lon, 6);
        ble.send(update);
    }
    
    // Auto mode
    if (autoMode && millis() - lastAutoTime > AUTO_MODE_INTERVAL) {
        lastAutoTime = millis();
        Serial.println("\n[AUTO] Triggering test...");
        sendTestPacket();
    }
    
    // Serial commands
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        handleCommand(cmd);
    }
    
    delay(10);

    // === DEEP SLEEP CHECK ===
    // If no emergencies are active, check if it's time to sleep
    if (emergency.getState() == EM_STATE_IDLE) {
        
        // Reset the timer if a button is being pressed or BLE is actively receiving data
        if (btnFlood.isLongPressed() || btnFire.isLongPressed() || btnCrime.isLongPressed()) {
            lastActivityTime = millis();
        }

        // If 3 minutes have passed with no activity...
        if (millis() - lastActivityTime > SLEEP_TIMEOUT) {
            Serial.println("\n💤 Activity timeout reached. Entering Deep Sleep...");
            ble.send("💤 Node entering Deep Sleep to save battery.");
            delay(500); // Give BLE time to send the message

            // Turn off LED and LoRa module
            led.off();
            LoRa.sleep();

            // 👇 THE S3 FIX: Tell the RTC processor to hold the pull-ups ON during sleep!
            rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_FLOOD);
            rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_FIRE);
            rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_CRIME);

            // Disable pull-downs just to be absolutely safe
            rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN_FLOOD);
            rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN_FIRE);
            rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN_CRIME);

            // Configure the Buttons to wake up the ESP32 (Active LOW)
            // Note: Use esp_sleep_enable_ext1_wakeup for S3, or esp_deep_sleep_enable_gpio_wakeup for C3.
            // Using a bitmask to listen to all 3 buttons simultaneously
            // 👇 FIX: Use the EXT1 controller for multiple pins
            uint64_t wakeMask = (1ULL << PIN_BTN_FLOOD) | (1ULL << PIN_BTN_FIRE) | (1ULL << PIN_BTN_CRIME);
            esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);

            // GO TO SLEEP
            esp_deep_sleep_start();
        }
    } else {
        // If an emergency is active, constantly reset the sleep timer!
        lastActivityTime = millis();
    }
}

// === HELPERS ===
void flashRGB(int r, int g, int b, int times) {
    led.flash(r, g, b, times);
    led.updateStatus(ble.isConnected(), gpsManager.isGPSValid());
}

uint8_t getBatteryLevel() {
   // 👇 NEW: Take 10 rapid samples and average them to eliminate ADC noise!
    uint32_t totalMilliVolts = 0;
    for (int i = 0; i < 10; i++) {
        totalMilliVolts += analogReadMilliVolts(PIN_BATTERY);
        delay(2); 
    }
    float pinMilliVolts = totalMilliVolts / 10.0;

    // Convert to Volts
    float pinVoltage = pinMilliVolts / 1000.0;
    
    // Multiply by 2 because our resistors cut the battery voltage in half
    float batteryVoltage = pinVoltage * 2.0;
    
    // Debug print so you can verify the reading in your Serial Monitor
    Serial.printf(">>> Battery Read: %.2fV\n", batteryVoltage);

    // Hard limits for a 3.7V LiPo
    if (batteryVoltage >= 4.20) return 100;
    if (batteryVoltage <= 3.20) return 0;
    
    // Accurate LiPo Discharge Curve Mapping
    float percentage;
    if (batteryVoltage >= 3.80) {
        // Top 50% of the battery is between 3.80V and 4.20V
        percentage = 50.0 + ((batteryVoltage - 3.80) / (4.20 - 3.80)) * 50.0;
    } else {
        // Bottom 50% drops extremely fast between 3.20V and 3.80V
        percentage = ((batteryVoltage - 3.20) / (3.80 - 3.20)) * 50.0;
    }
    
    return (uint8_t)percentage;
}

void sendTestPacket() {
    float lat, lon;
    gpsManager.determineLocation(lat, lon, true);
    
    SafeChainPacket pkt;
    PacketBuilder::build(pkt, nodeID.c_str(), MSG_TEST, 
                         EM_NONE, lat, lon, getBatteryLevel());
                         
    router.cancelRelay();  // Priority override
    
    // 👇 Update Terminal and BLE
    Serial.println(">>> SENDING TEST...");
    ble.send("📡 SENDING TEST PING...");
    
    flashRGB(255, 255, 255, 2);
    tone(PIN_BUZZER, 3000, 100); // Quick chirp
    
    LoRa.idle();
    LoRa.beginPacket();
    LoRa.write((uint8_t*)&pkt, sizeof(SafeChainPacket));
    LoRa.endPacket(); 
    
    LoRa.receive();
    
    // 👇 Update Terminal and BLE
    Serial.println(">>> SENT!");
    ble.send("✅ TEST PACKET AIRBORNE!");
}

//

// void triggerFlood() {
//     Serial.println("\n🚨 FLOOD DETECTED!");
//     ble.send("🚨 FLOOD");
//     led.flash(0, 0, 255, 2);
    
//     // Quick, single sonar ping
//     tone(PIN_BUZZER, 2000, 400); 
//     delay(400);

//     float lat, lon;
//     gpsManager.determineLocation(lat, lon, true);
//     emergency.trigger(EM_FLOOD, lat, lon, getBatteryLevel());
// }

// void triggerFire() {
//     Serial.println("\n🚨 FIRE DETECTED!");
//     ble.send("🚨 FIRE");
//     led.flash(255, 50, 0, 2);
    
//     // Just ONE cycle of the European Hi-Lo pattern
//     tone(PIN_BUZZER, 1000, 300); delay(300); 
//     tone(PIN_BUZZER, 2500, 300); delay(300); 

//     float lat, lon;
//     gpsManager.determineLocation(lat, lon, true);
//     emergency.trigger(EM_FIRE, lat, lon, getBatteryLevel());
// }

// void triggerCrime() {
//     Serial.println("\n🚨 CRIME DETECTED!");
//     ble.send("🚨 CRIME");
//     led.flash(255, 0, 0, 2);
    
//     // One quick, aggressive sweep (Police Yelp)
//     for(int f=1000; f<=3500; f+=200) { 
//         tone(PIN_BUZZER, f, 20);
//         delay(20); 
//     }

//     float lat, lon;
//     gpsManager.determineLocation(lat, lon, true);
//     emergency.trigger(EM_CRIME, lat, lon, getBatteryLevel());
// }

// Button Sirens code

void triggerFlood() {
    Serial.println("\n🚨 FLOOD DETECTED!");
    ble.send("🚨 FLOOD");
    led.flash(0, 0, 255, 2);

    // Siren: Slow "Sonar" Ping (High Pitch)
    for(int i=0; i<3; i++) { 
      tone(PIN_BUZZER, 2000, 300); 
      delay(600); 
    }

    float lat, lon;
    gpsManager.determineLocation(lat, lon, true);
    emergency.trigger(EM_FLOOD, lat, lon, getBatteryLevel());
}

void triggerFire() {
    Serial.println("\n🚨 FIRE DETECTED!");
    ble.send("🚨 FIRE");
    led.flash(255, 50, 0, 2);

    // Siren: Fast European Hi-Lo pattern
    for(int i=0; i<4; i++) { 
      tone(PIN_BUZZER, 1000, 300); delay(300); 
      tone(PIN_BUZZER, 2500, 300); delay(300); 
    }

    float lat, lon;
    gpsManager.determineLocation(lat, lon, true);
    emergency.trigger(EM_FIRE, lat, lon, getBatteryLevel());
}

void triggerCrime() {
    Serial.println("\n🚨 CRIME DETECTED!");
    ble.send("🚨 CRIME");
    led.flash(255, 0, 0, 2);

    // Siren: Aggressive fast sweep (Police Yelp)
    for(int i=0; i<4; i++) {
      for(int f=1000; f<=3500; f+=100) { 
        tone(PIN_BUZZER, f, 15); delay(15); 
      }
    }

    float lat, lon;
    gpsManager.determineLocation(lat, lon, true);
    emergency.trigger(EM_CRIME, lat, lon, getBatteryLevel());
}

void triggerSafe() {

    // 👇 FIX 1: Instantly kill the GPS Refinement Watchdog!
    needsGPSRefine = false;

    // Only allow marking "SAFE" if an alert is currently active or retrying
    if (emergency.getState() != EM_STATE_IDLE) {
        Serial.println("\n✅ SAFE SIGNAL INITIATED!");
        ble.send("✅ MARKED SAFE - Cancelling Alert...");
        
        // Flash Green to indicate safety
        led.flash(0, 255, 0, 3);
        
        // Play a happy "All Clear" double-tone
        tone(PIN_BUZZER, 1000, 150); delay(150);
        tone(PIN_BUZZER, 2000, 150);
        
        float lat, lon;
        gpsManager.determineLocation(lat, lon, true);
        
        // This will override the current emergency and broadcast EM_SAFE (0x04) to the Gateway!
        emergency.trigger(EM_SAFE, lat, lon, getBatteryLevel());
    } else {
        Serial.println("\nℹ️ System is already idle. Safe signal ignored.");
    }
}


void onGPSInject(float lat, float lon) {
    gpsManager.injectBLE(lat, lon);
}

void onBLECommand(String cmd) {
    Serial.printf("[BLE CMD]: %s\n", cmd.c_str());
    handleCommand(cmd);
}

void handleCommand(String input) {
    input.trim();
    input.toLowerCase();
    
    int spaceIdx = input.indexOf(' ');
    String cmd = (spaceIdx == -1) ? input : input.substring(0, spaceIdx);
    String arg = (spaceIdx == -1) ? "" : input.substring(spaceIdx + 1);
    
    String response = "";
    
    if (cmd == "flood") { triggerFlood(); }
    else if (cmd == "fire") { triggerFire(); }
    else if (cmd == "crime") { triggerCrime(); }
    else if (cmd == "send") { sendTestPacket(); }
    else if (cmd == "gps") {
        float lat, lon;
        gpsManager.determineLocation(lat, lon, true);
        response = "GPS: " + String(lat, 6) + ", " + String(lon, 6) + 
                   " (" + gpsManager.getSourceName() + ")";
    }
    else if (cmd == "relay") {
        if (arg == "on") {
            router.setRelayEnabled(true);
            storage.setRelayEnabled(true);
            response = "Relay: ON";
        } else if (arg == "off") {
            router.setRelayEnabled(false);
            storage.setRelayEnabled(false);
            response = "Relay: OFF";
        } else {
            response = "Relay: " + String(router.getRelayEnabled() ? "ON" : "OFF");
        }
    }
    else if (cmd == "uid") {
        if (arg.length() > 0) {
            nodeID = arg.substring(0, 5);  // Max 5 chars
            storage.setNodeID(nodeID);
            response = "UID saved: " + nodeID + " (reboot required)";
        } else {
            response = "Current UID: " + nodeID;
        }
    }
    else if (cmd == "sf") {
        if (arg.length() > 0) {
            uint8_t sf = arg.toInt();
            if (sf >= 7 && sf <= 12) {
                storage.setSpreadingFactor(sf);
                LoRa.setSpreadingFactor(sf);
                response = "SF set to " + String(sf);
            }
        } else {
            response = "Current SF: " + String(storage.getSpreadingFactor());
        }
    }
    else if (cmd == "auto") {
        if (arg == "on") {
            autoMode = true;
            response = "Auto mode: ON";
        } else if (arg == "off") {
            autoMode = false;
            response = "Auto mode: OFF";
        } else {
            response = "Auto mode: " + String(autoMode ? "ON" : "OFF");
        }
    }
    else if (cmd == "info") {
        response = "=== NODE INFO ===\n";
        response += "UID: " + nodeID + "\n";
        response += "MAC: " + ble.getMacAddress() + "\n"; // 👇 ADD THIS LINE
        response += "Relay: " + String(router.getRelayEnabled() ? "ON" : "OFF") + "\n";
        response += "SF: " + String(storage.getSpreadingFactor()) + "\n";
        response += "BLE: " + String(ble.isConnected() ? "Connected" : "Disconnected") + "\n";
        response += "GPS: " + String(gpsManager.isGPSValid() ? "Valid" : "No Fix");
    }
    else if (cmd == "reboot") {
        response = "Rebooting...";
        Serial.println(response);
        ble.send(response);
        delay(1000);
        ESP.restart();
    }
    else {
        response = "Unknown: " + cmd;
        printMenu();
    }
    
    if (response != "") {
        Serial.println(response);
        ble.send(response);
    }
}

void printMenu() {
    Serial.println("\n--- COMMANDS ---");
    Serial.println("flood | fire | crime | send");
    Serial.println("gps | relay on/off | uid <id>");
    Serial.println("sf <7-12> | auto on/off | info | reboot");
}

