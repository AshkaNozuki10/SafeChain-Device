#include "gps_manager.h"
#include "config.h"

GPSManager::GPSManager() 
    : bleLat(0), bleLon(0), bleValid(false),
      finalLat(0), finalLon(0), source(LOC_NONE) {
    gpsSerial = new HardwareSerial(1);
}

void GPSManager::init() {
    gpsSerial->begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    Serial.println("✅ GPS Manager Ready");
}

void GPSManager::update() {
    while (gpsSerial->available() > 0) {
        gps.encode(gpsSerial->read());
    }
}

void GPSManager::injectBLE(float lat, float lon) {
    bleLat = lat;
    bleLon = lon;
    bleValid = true;
    Serial.printf(">>> BLE GPS Injected: %.6f, %.6f\n", lat, lon);
}

void GPSManager::determineLocation(float &lat, float &lon, bool verbose) {
    // Priority 1: Hardware GPS (fresh)
    if (gps.location.isValid() && gps.location.age() < GPS_VALID_AGE_MS) {
        finalLat = gps.location.lat();
        finalLon = gps.location.lng();
        source = LOC_GPS_HARDWARE;
        if (verbose) Serial.printf("GPS: %.6f, %.6f\n", finalLat, finalLon);
    }
    // Priority 2: BLE Phone GPS
    else if (bleValid) {
        finalLat = bleLat;
        finalLon = bleLon;
        source = LOC_BLE_PHONE;
        if (verbose) Serial.printf("BLE GPS: %.6f, %.6f\n", finalLat, finalLon);
    }
    // Fallback: None
    else {
        finalLat = 0.0;
        finalLon = 0.0;
        source = LOC_NONE;
        if (verbose) Serial.println("No Location Available");
    }
    
    lat = finalLat;
    lon = finalLon;
}

bool GPSManager::isGPSValid() const {
    return gps.location.isValid();
}

const char* GPSManager::getSourceName() const {
    switch(source) {
        case LOC_GPS_HARDWARE: return "GPS";
        case LOC_BLE_PHONE: return "BLE";
        default: return "NONE";
    }
}