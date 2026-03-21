#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include <Preferences.h>
#include "config.h"

class Storage {
private:
    Preferences prefs;
    
public:
    Storage();
    
    void init();
    
    // Node ID
    String getNodeID();
    void setNodeID(const String &id);
    
    // Relay mode
    bool getRelayEnabled();
    void setRelayEnabled(bool enabled);
    
    // Spreading Factor
    uint8_t getSpreadingFactor();
    void setSpreadingFactor(uint8_t sf);
};

#endif