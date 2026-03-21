#ifndef GPS_MANAGER_H
#define GPS_MANAGER_H

#include <Arduino.h>
#include <TinyGPS++.h>
#include "config.h"

enum LocationSource {
    LOC_NONE,
    LOC_GPS_HARDWARE,
    LOC_BLE_PHONE
};

class GPSManager {
private:
    TinyGPSPlus gps;
    HardwareSerial *gpsSerial;
    
    // BLE injected coordinates
    float bleLat;
    float bleLon;
    bool bleValid;
    
    // Final determined location
    float finalLat;
    float finalLon;
    LocationSource source;
    
public:
    GPSManager();
    
    void init();
    void update();  // Read GPS serial
    
    // BLE GPS injection
    void injectBLE(float lat, float lon);
    
    // Get best location
    void determineLocation(float &lat, float &lon, bool verbose = false);
    
    // Status queries
    bool isGPSValid() const;
    LocationSource getSource() const { return source; }
    const char* getSourceName() const;
};

#endif