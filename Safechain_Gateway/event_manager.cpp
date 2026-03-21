#include "event_manager.h"
#include "packet.h"
#include "config.h"
#include <Arduino.h>
#include <LoRa.h>
#include <cstring>

// === CONSTRUCTOR & INIT ===

EventManager::EventManager() 
    : eventIndex(0), nodeCount(0), cacheIndex(0), dedupIndex(0),
      totalPackets(0), duplicatePackets(0), corruptedPackets(0) {
    memset(events, 0, sizeof(events));
    memset(nodes, 0, sizeof(nodes));
    memset(seenCache, 0, sizeof(seenCache));
    memset(recentEmergencies, 0, sizeof(recentEmergencies));
}

void EventManager::init() {
    Serial.println("✅ Event Manager Ready");
}

// === SEQUENCE DEDUPLICATION (RETRIES) ===

bool EventManager::isDuplicate(const char* srcID, uint16_t seqNum) {
    for (int i = 0; i < DUPLICATE_CACHE_SIZE; i++) {
        if (seenCache[i].seqNum == seqNum && strcmp(seenCache[i].srcID, srcID) == 0) {
            return true;
        }
    }
    return false;
}

void EventManager::markSeen(const char* srcID, uint16_t seqNum) {
    strncpy(seenCache[cacheIndex].srcID, srcID, 5);
    seenCache[cacheIndex].srcID[5] = '\0';
    seenCache[cacheIndex].seqNum = seqNum;
    cacheIndex = (cacheIndex + 1) % DUPLICATE_CACHE_SIZE;
}

// === TIME-BASED DEDUPLICATION (SPAM PROTECTION) ===

bool EventManager::isRecentDuplicate(const SafeChainPacket &pkt) {
    if (pkt.msgType < EM_FIRE || pkt.msgType > EM_CRIME) {
        return false;
    }
    
    // We increased this to 40 in the header to support 30 nodes
    for (int i = 0; i < 40; i++) {
        if (strcmp(recentEmergencies[i].srcID, pkt.srcID) == 0 &&
            recentEmergencies[i].msgType == pkt.msgType &&
            (millis() - recentEmergencies[i].timestamp) < DEDUP_WINDOW_MS) {
            
            Serial.printf(">>> DEDUP: Same emergency from %s within 30s\n", pkt.srcID);
            return true;
        }
    }
    return false;
}

void EventManager::markRecentEmergency(const SafeChainPacket &pkt) {
    strncpy(recentEmergencies[dedupIndex].srcID, pkt.srcID, 5);
    recentEmergencies[dedupIndex].srcID[5] = '\0';
    recentEmergencies[dedupIndex].msgType = pkt.msgType;
    recentEmergencies[dedupIndex].timestamp = millis();
    dedupIndex = (dedupIndex + 1) % 40; 
}

// === CORE PACKET PROCESSING ===

bool EventManager::processPacket(SafeChainPacket &pkt) {
    totalPackets++;
    
    // 1. Check sequence-based duplicate (retries)
    if (isDuplicate(pkt.srcID, pkt.seqNum)) {
        duplicatePackets++;
        Serial.println(">>> Duplicate packet - Logged but not displayed");
        return false;
    }
    
    // 2. Check time-window duplicate (multiple frantic presses)
    if (isRecentDuplicate(pkt)) {
        Serial.println(">>> Suppressed: Same emergency within 30s");
        logCSV(pkt);  // Still log for analysis
        
        // CRITICAL FIX: The Gateway must still ACK the Node so it stops retrying!
        if (pkt.msgType >= EM_FIRE && pkt.msgType <= EM_CRIME) {
            sendACK(pkt);
        }
        return false;
    }
    
    // Track both types of deduplication
    markSeen(pkt.srcID, pkt.seqNum);
    if (pkt.msgType >= EM_FIRE && pkt.msgType <= EM_CRIME) {
        markRecentEmergency(pkt);
    }
    
    // Update node statistics
    updateNodeStats(pkt);
    
    // Display alert
    displayAlert(pkt);
    
    // Log to CSV
    logCSV(pkt);
    
    // Store event
    if (pkt.msgType != MSG_ACK && pkt.msgType != MSG_TEST) {
        storeEvent(pkt);
    }
    
    // Send ACK for emergencies (FIRE, FLOOD, CRIME)
    if (pkt.msgType >= EM_FIRE && pkt.msgType <= EM_CRIME) {
        sendACK(pkt);
    }
    
    return true;
}

// === RESPONSE ROUTINES ===

void EventManager::sendACK(const SafeChainPacket &pkt) {
    SafeChainPacket ackPkt;
    PacketBuilder::buildACK(ackPkt, pkt.srcID, pkt.seqNum);
    
    // Random backoff to prevent Gateway-Repeater collisions
    delay(random(100, 300));
    
    Serial.printf(">>> SENDING ACK TO: %s (Seq: %u)\n", pkt.srcID, pkt.seqNum);
    
    LoRa.beginPacket();
    LoRa.write((uint8_t*)&ackPkt, sizeof(SafeChainPacket));
    LoRa.endPacket(); // Blocking send
    
    LoRa.receive(); // Turn ears back on
}

// === DISPLAY & LOGGING ===

void EventManager::displayAlert(const SafeChainPacket &pkt) {
    Serial.println("\n🚨 ================= EMERGENCY ALERT ================= 🚨");
    
    // 1. Print the exact trigger words the Python script is looking for
    if (pkt.msgType == EM_FIRE) {
        Serial.println("   ** FIRE ALARM **");
    } else if (pkt.msgType == EM_FLOOD) {
        Serial.println("   ** FLOOD ALERT **");
    } else if (pkt.msgType == EM_CRIME) {
        Serial.println("   ** CRIME / SOS **");
    } else if (pkt.msgType == EM_SAFE) {
        Serial.println("   ** USER MARKED SAFE **");
    } else {
        Serial.println("   ** UNKNOWN **");
    }

    // 2. Format Node ID and Location exactly as Python expects (with the spaces before the colon)
    Serial.printf("   NODE ID : %s\n", pkt.srcID);
    Serial.printf("   LOCATION : %.6f, %.6f\n", pkt.latitude, pkt.longitude);
    
    // 3. Extra info (Python script ignores these, but good for local terminal viewing)
    Serial.printf("   BATTERY:  %u%%\n", pkt.battery);
    Serial.printf("   NETWORK:  RSSI %d dBm | Hops: %u\n", pkt.rssi, pkt.hopCount);
    Serial.println("=========================================================\n");
}

void EventManager::logCSV(const SafeChainPacket &pkt) {
    // Format: CSV,Timestamp,NodeID,MsgType,SeqNum,Lat,Lon,Hops,Battery,RSSI
    Serial.printf("CSV,%lu,%s,%02X,%u,%.6f,%.6f,%u,%u,%d\n", 
        millis(), pkt.srcID, pkt.msgType, pkt.seqNum, 
        pkt.latitude, pkt.longitude, pkt.hopCount, pkt.battery, pkt.rssi);
}

void EventManager::storeEvent(const SafeChainPacket &pkt) {
    EmergencyEvent &evt = events[eventIndex];
    evt.eventID = millis(); // Simple unique ID
    strncpy(evt.srcID, pkt.srcID, 5);
    evt.srcID[5] = '\0';
    evt.msgType = pkt.msgType;
    evt.seqNum = pkt.seqNum;
    evt.latitude = pkt.latitude;
    evt.longitude = pkt.longitude;
    evt.hopCount = pkt.hopCount;
    evt.battery = pkt.battery;
    evt.rssi = pkt.rssi;
    evt.timestamp = millis();
    evt.active = true;
    
    eventIndex = (eventIndex + 1) % MAX_EVENTS;
}

// === NODE TRACKING & STATS ===

NodeStats* EventManager::findNode(const char* nodeID) {
    for (int i = 0; i < nodeCount; i++) {
        if (strcmp(nodes[i].nodeID, nodeID) == 0) return &nodes[i];
    }
    // Note: We changed this to 40 in the header to support 30 nodes safely
    if (nodeCount < 40) { 
        strncpy(nodes[nodeCount].nodeID, nodeID, 5);
        nodes[nodeCount].nodeID[5] = '\0';
        nodes[nodeCount].packetsReceived = 0;
        nodes[nodeCount].packetsMissed = 0;
        nodes[nodeCount].avgRSSI = 0;
        nodes[nodeCount].avgHops = 0;
        nodeCount++;
        return &nodes[nodeCount - 1];
    }
    return nullptr;
}

void EventManager::updateNodeStats(const SafeChainPacket &pkt) {
    NodeStats* node = findNode(pkt.srcID);
    if (!node) return;
    
    // Check for missed packets based on sequence numbers
    if (node->packetsReceived > 0 && pkt.seqNum > node->lastSeq + 1) {
        node->packetsMissed += (pkt.seqNum - node->lastSeq - 1);
    }
    
    node->packetsReceived++;
    node->lastSeq = pkt.seqNum;
    node->lastSeen = millis();
    
    // Moving average for RSSI and Hops
    if (node->packetsReceived == 1) {
        node->avgRSSI = pkt.rssi;
        node->avgHops = pkt.hopCount;
    } else {
        node->avgRSSI = (node->avgRSSI * 0.8) + (pkt.rssi * 0.2);
        node->avgHops = (node->avgHops * 0.8) + (pkt.hopCount * 0.2);
    }
}

// === HELPERS ===

const char* EventManager::getTypeName(uint8_t type) {
    if (type >= EM_FIRE && type <= EM_CRIME) {
        switch(type) {
            case EM_FIRE: return "FIRE";
            case EM_FLOOD: return "FLOOD";
            case EM_CRIME: return "CRIME";
            case EM_SAFE: return "SAFE";
            default: return "EMERGENCY";
        }
    }
    switch(type) {
        case MSG_ACK: return "ACK";
        case MSG_HEARTBEAT: return "HEARTBEAT";
        case MSG_TEST: return "TEST";
        default: return "UNKNOWN";
    }
}

void EventManager::printStats() {
    Serial.println("\n=== GATEWAY STATISTICS ===");
    Serial.printf("Total Packets:      %lu\n", totalPackets);
    Serial.printf("Duplicate Packets:  %lu\n", duplicatePackets);
    Serial.printf("Corrupted Packets:  %lu\n", corruptedPackets);
    Serial.printf("Active Nodes:       %u\n", nodeCount);
    Serial.println("==========================\n");
}

void EventManager::printPDR() {
    Serial.println("\n=== PACKET DELIVERY RATIO (PDR) ===");
    for (int i = 0; i < nodeCount; i++) {
        NodeStats &node = nodes[i];
        uint16_t total = node.packetsReceived + node.packetsMissed;
        float pdr = (total > 0) ? (float)node.packetsReceived / total * 100.0 : 0;
        
        Serial.printf("Node %s:\n", node.nodeID);
        Serial.printf("  Received: %u | Missed: %u | PDR: %.1f%%\n", node.packetsReceived, node.packetsMissed, pdr);
        Serial.printf("  Avg RSSI: %d dBm | Avg Hops: %.1f\n", node.avgRSSI, node.avgHops);
    }
    Serial.println("===================================\n");
}