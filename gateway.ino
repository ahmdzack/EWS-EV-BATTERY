/*
 * ═══════════════════════════════════════════════════════════
 *  LoRa32 BMS Gateway - Simple Version (No WiFi)
 *  Version: 3.0 SIMPLE
 *  
 *  Hardware:   TTGO LoRa32 v2.1
 *  Function:  Receive BMS + GPS + Battery data via LoRa
 *  
 *  Multi-Gateway Support:  Manual ID setting
 *  No WiFi, No MAC address detection
 * ═══════════════════════════════════════════════════════════
 */

#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ═══════════════════════════════════════════════════════════
// ⚠️⚠️⚠️ EDIT SECTION - GATEWAY CONFIGURATION ⚠️⚠️⚠️
// ═══════════════════════════════════════════════════════════

#define GATEWAY_ID        1              // ✅ GANTI: 1, 2, 3, atau 4
#define GATEWAY_NAME      "GW-North"     // ✅ GANTI: Nama Gateway
#define GATEWAY_LOCATION  "Building A"   // ✅ GANTI: Lokasi Gateway

// ═══════════════════════════════════════════════════════════
// HARDWARE CONFIGURATION
// ═══════════════════════════════════════════════════════════

// OLED Display
#define OLED_SDA    21
#define OLED_SCL    22
#define OLED_RST    -1
#define OLED_ADDR   0x3C

// LED Indicator
#define LED_PIN     25

// LoRa Pins
#define SCK_LORA    5
#define MISO_LORA   19
#define MOSI_LORA   27
#define SS_LORA     18
#define RST_LORA    23      // Try 14 if 23 doesn't work
#define DIO0_LORA   26

// LoRa Frequency
#define LORA_BAND   923E6

// ═══════════════════════════════════════════════════════════
// DATA STRUCTURES
// ═══════════════════════════════════════════════════════════

struct BMSData {
    float voltage;
    float current;
    float soc;
    float capacity;
    bool valid;
};

struct GPSData {
    double latitude;
    double longitude;
    float speed;
    int satellites;
    bool valid;
};

struct NodeBatteryData {
    float voltage;
    int percent;
    bool charging;
    bool valid;
};

struct SignalQuality {
    int rssi;
    float snr;
};

// ═══════════════════════════════════════════════════════════
// GLOBAL VARIABLES
// ═══════════════════════════════════════════════════════════

Adafruit_SSD1306 display(128, 64, &Wire, OLED_RST);

BMSData bms = {0};
GPSData gps = {0};
NodeBatteryData nodeBattery = {0};
SignalQuality signalQuality = {0};

unsigned long lastPacketTime = 0;
unsigned long packetCount = 0;
unsigned long errorCount = 0;
unsigned long startTime = 0;

bool systemReady = false;
bool oledAvailable = false;

// ═══════════════════════════════════════════════════════════
// DISPLAY FUNCTIONS
// ═══════════════════════════════════════════════════════════

void updateDisplay() {
    if (!oledAvailable) return;
    
    display.clearDisplay();
    display.setTextColor(WHITE);
    
    // ═══════════════════════════════════════════════════════════
    // LINE 1: Gateway ID & Location
    // ═══════════════════════════════════════════════════════════
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.printf("GW%d:", GATEWAY_ID);
    
    // Location (truncated to fit)
    String loc = String(GATEWAY_LOCATION);
    if (loc.length() > 13) {
        loc = loc. substring(0, 13);
    }
    display.print(loc);
    
    // GPS satellites indicator (right side)
    if (gps.valid && gps.satellites > 0) {
        display.setCursor(110, 0);
        display.printf("S%d", gps.satellites);
    }
    
    // ═══════════════════════════════════════════════════════════
    // LINE 2: Packet Count & Node Battery
    // ═══════════════════════════════════════════════════════════
    display.setCursor(0, 10);
    display.printf("Pkt:%lu", packetCount);
    
    // Node battery (right side)
    if (nodeBattery.valid) {
        display.setCursor(70, 10);
        if (nodeBattery.charging) {
            display.print("⚡");
        } else {
            if (nodeBattery. percent > 75) display.print("█");
            else if (nodeBattery.percent > 50) display.print("▓");
            else if (nodeBattery.percent > 25) display.print("▒");
            else display.print("░");
        }
        display.printf("%d%%", nodeBattery.percent);
    }
    
    // ═══════════════════════════════════════════════════════════
    // LINE 3-4: BMS Data (Large Font)
    // ═══════════════════════════════════════════════════════════
    display.setTextSize(2);
    
    // Voltage
    display.setCursor(0, 24);
    display.printf("%.1fV", bms.voltage);
    
    // SOC (State of Charge)
    display.setCursor(75, 24);
    display.printf("%.0f%%", bms.soc);
    
    // ═══════════════════════════════════════════════════════════
    // LINE 5: Current (Large Font)
    // ═══════════════════════════════════════════════════════════
    display.setCursor(0, 42);
    if (bms.current > 0.1) {
        display.printf("+%.1fA", bms.current);  // Charging
    } else if (bms.current < -0.1) {
        display.printf("%. 1fA", bms.current);   // Discharging
    } else {
        display.print("0.0A");  // Idle
    }
    
    // ═══════════════════════════════════════════════════════════
    // LINE 6: Status & Signal Quality
    // ═══════════════════════════════════════════════════════════
    display.setTextSize(1);
    display.setCursor(0, 56);
    
    // Connection status
    unsigned long timeSince = (millis() - lastPacketTime) / 1000;
    
    if (! systemReady) {
        display.print("WAIT");
    } else if (timeSince < 10) {
        display.print("ON");
    } else if (timeSince < 60) {
        display.printf("%lus", timeSince);
    } else {
        display. printf("%lum", timeSince / 60);
    }
    
    // GPS Speed or Capacity (middle)
    display.setCursor(35, 56);
    if (gps.valid && gps. speed > 1.0) {
        display.printf("%.0fkm/h", gps.speed);
    } else if (bms.valid) {
        display.printf("%. 1fAh", bms.capacity);
    }
    
    // RSSI Signal Strength (right side)
    display.setCursor(100, 56);
    if (signalQuality.rssi > -50) {
        display.print("****");      // Excellent
    } else if (signalQuality.rssi > -70) {
        display.print("***");       // Good
    } else if (signalQuality.rssi > -90) {
        display.print("**");        // Fair
    } else if (signalQuality.rssi > -110) {
        display.print("*");         // Poor
    } else {
        display.print("-");         // No signal
    }
    
    display.display();
}
void showSplashScreen() {
    if (! oledAvailable) return;
    
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(WHITE);
    display.setCursor(5, 5);
    display.println("LoRa32");
    
    display.setTextSize(1);
    display.setCursor(10, 30);
    display.printf("Gateway #%d", GATEWAY_ID);
    
    display.setCursor(10, 45);
    display.println("Starting...");
    
    display.display();
}

void showError(const char* errorMsg) {
    if (!oledAvailable) return;
    
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0, 0);
    display.println("ERROR!");
    display.println();
    display.println(errorMsg);
    display.display();
}

// ═══════════════════════════════════════════════════════════
// PARSING FUNCTIONS
// ═══════════════════════════════════════════════════════════

bool parseLoRaPacket(String packet) {
    // Format: BMS,Volt,Amp,SOC,Cap,Lat,Lng,Speed,Sats,BatV,BatPct,Charging
    
    if (! packet.startsWith("BMS,")) {
        Serial.println("[PARSE] Invalid header");
        return false;
    }
    
    // Count commas
    int commaCount = 0;
    for (unsigned int i = 0; i < packet.length(); i++) {
        if (packet[i] == ',') commaCount++;
    }
    
    if (commaCount < 4) {
        Serial.println("[PARSE] Insufficient fields");
        return false;
    }
    
    // Parse BMS data (mandatory)
    int idx1 = packet.indexOf(',', 0);
    int idx2 = packet.indexOf(',', idx1 + 1);
    int idx3 = packet. indexOf(',', idx2 + 1);
    int idx4 = packet.indexOf(',', idx3 + 1);
    int idx5 = packet.indexOf(',', idx4 + 1);
    
    if (idx1 == -1 || idx2 == -1 || idx3 == -1 || idx4 == -1) {
        Serial.println("[PARSE] BMS field error");
        return false;
    }
    
    bms.voltage = packet.substring(idx1 + 1, idx2).toFloat();
    bms.current = packet.substring(idx2 + 1, idx3).toFloat();
    bms.soc = packet.substring(idx3 + 1, idx4).toFloat();
    
    if (idx5 > 0) {
        bms.capacity = packet.substring(idx4 + 1, idx5).toFloat();
    } else {
        bms.capacity = packet.substring(idx4 + 1).toFloat();
    }
    
    // Validate BMS data
    if (bms.voltage < 0 || bms.voltage > 100 || bms.soc < 0 || bms.soc > 100) {
        Serial.println("[PARSE] BMS out of range");
        return false;
    }
    
    bms.valid = true;
    
    // Parse GPS data (optional)
    if (commaCount >= 8 && idx5 > 0) {
        int idx6 = packet.indexOf(',', idx5 + 1);
        int idx7 = packet.indexOf(',', idx6 + 1);
        int idx8 = packet.indexOf(',', idx7 + 1);  // ✅ FIX: Declare idx8
        int idx9 = -1;  // Initialize for next section
        
        if (idx6 > 0 && idx7 > 0) {
            gps.latitude = packet.substring(idx5 + 1, idx6).toDouble();
            gps.longitude = packet.substring(idx6 + 1, idx7).toDouble();
            
            if (idx8 > 0) {
                gps. speed = packet.substring(idx7 + 1, idx8).toFloat();
                gps. satellites = packet.substring(idx8 + 1).toInt();
            }
            
            gps.valid = (gps.latitude != 0.0 || gps.longitude != 0.0);
            
            // Now idx9 can be calculated
            idx9 = packet.indexOf(',', idx8 + 1);
        }
        
        // Parse Node Battery data (optional - field 9, 10, 11)
        if (commaCount >= 11 && idx9 > 0) {
            int idx10 = packet.indexOf(',', idx9 + 1);
            int idx11 = packet.indexOf(',', idx10 + 1);
            
            if (idx10 > 0) {
                nodeBattery.voltage = packet.substring(idx9 + 1, idx10).toFloat();
                nodeBattery.percent = packet.substring(idx10 + 1, idx11 > 0 ? idx11 :  packet.length()).toInt();
                
                if (idx11 > 0) {
                    nodeBattery.charging = (packet.substring(idx11 + 1).toInt() == 1);
                }
                
                nodeBattery.valid = true;
            }
        } else {
            nodeBattery.valid = false;
        }
    } else {
        gps.valid = false;
        nodeBattery.valid = false;
    }
    
    return true;
}

// ═══════════════════════════════════════════════════════════
// OUTPUT FUNCTIONS
// ═══════════════════════════════════════════════════════════

void printSerialData() {
    Serial.println("┌──────────── BMS DATA ────────────┐");
    Serial.printf("│ Voltage    : %7.2f V          │\n", bms.voltage);
    Serial.printf("│ Current    : %7.2f A          │", bms.current);
    
    if (bms.current > 0.1) {
        Serial.println(" ⚡");
    } else if (bms.current < -0.1) {
        Serial.println(" ⬇");
    } else {
        Serial.println(" ⏸");
    }
    
    Serial.printf("│ SOC        : %7.1f %%          │\n", bms.soc);
    Serial.printf("│ Capacity   :  %7.2f Ah         │\n", bms.capacity);
    Serial.println("└──────────────────────────────────┘");
    
    if (gps.valid) {
        Serial.println("┌──────────── GPS DATA ────────────┐");
        Serial.printf("│ Latitude   : %12.6f      │\n", gps.latitude);
        Serial.printf("│ Longitude  : %12.6f      │\n", gps.longitude);
        Serial.printf("│ Speed      : %7.1f km/h        │\n", gps.speed);
        Serial.printf("│ Satellites : %7d            │\n", gps.satellites);
        Serial.println("└──────────────────────────────────┘");
        Serial.printf("📍 Maps: https://maps.google.com/?q=%.6f,%.6f\n", 
                      gps.latitude, gps. longitude);
    }
    
    if (nodeBattery. valid) {
        Serial.println("┌─────── NODE BATTERY ─────────────┐");
        Serial.printf("│ Voltage    : %7.2f V          │\n", nodeBattery.voltage);
        Serial.printf("│ Percent    : %7d %%          │", nodeBattery.percent);
        if (nodeBattery.charging) {
            Serial.println(" ⚡");
        } else {
            Serial.println();
        }
        Serial.println("└──────────────────────────────────┘");
    }
    
    Serial.println("┌─────────── SIGNAL ───────────────┐");
    Serial.printf("│ RSSI       : %7d dBm        │\n", signalQuality.rssi);
    Serial.printf("│ SNR        :  %7.1f dB         │\n", signalQuality.snr);
    Serial.println("└──────────────────────────────────┘");
}

void printJSON() {
    Serial.print("{");
    Serial.print("\"gateway_id\":");
    Serial.print(GATEWAY_ID);
    Serial.print(",\"gateway_name\": \"");
    Serial.print(GATEWAY_NAME);
    Serial.print("\",\"gateway_location\":\"");
    Serial.print(GATEWAY_LOCATION);
    Serial.print("\",\"timestamp\":");
    Serial.print(millis() / 1000);
    Serial.print(",\"packet_number\":");
    Serial.print(packetCount);
    Serial.print(",\"uptime\":");
    Serial.print((millis() - startTime) / 1000);
    Serial.print(",\"voltage\":");
    Serial.print(bms.voltage, 2);
    Serial.print(",\"current\":");
    Serial.print(bms.current, 2);
    Serial.print(",\"soc\":");
    Serial.print(bms.soc, 1);
    Serial.print(",\"capacity\":");
    Serial.print(bms.capacity, 2);
    
    if (gps.valid) {
        Serial.print(",\"latitude\":");
        Serial.print(gps.latitude, 6);
        Serial.print(",\"longitude\":");
        Serial.print(gps.longitude, 6);
        Serial.print(",\"speed\":");
        Serial.print(gps.speed, 1);
        Serial.print(",\"satellites\":");
        Serial.print(gps.satellites);
    }
    
    if (nodeBattery.valid) {
        Serial.print(",\"node_battery_voltage\":");
        Serial.print(nodeBattery.voltage, 2);
        Serial.print(",\"node_battery_percent\":");
        Serial.print(nodeBattery.percent);
        Serial.print(",\"node_battery_charging\":");
        Serial.print(nodeBattery. charging ? "true" : "false");
    }
    
    Serial.print(",\"rssi\":");
    Serial.print(signalQuality.rssi);
    Serial.print(",\"snr\":");
    Serial.print(signalQuality.snr, 1);
    Serial.println("}");
}

// ═══════════════════════════════════════════════════════════
// LORA HANDLER
// ═══════════════════════════════════════════════════════════

void onLoRaReceive(int packetSize) {
    if (packetSize == 0) return;
    
    digitalWrite(LED_PIN, HIGH);
    
    String packet = "";
    while (LoRa.available()) {
        packet += (char)LoRa.read();
    }
    
    signalQuality.rssi = LoRa.packetRssi();
    signalQuality.snr = LoRa.packetSnr();
    
    Serial.println();
    Serial.println("═══════════════════════════════════════════════════");
    Serial.printf("📦 [GATEWAY #%d] PACKET #%lu\n", GATEWAY_ID, packetCount + 1);
    Serial.println("═══════════════════════════════════════════════════");
    Serial.print("RAW: ");
    Serial.println(packet);
    Serial.println();
    
    if (parseLoRaPacket(packet)) {
        systemReady = true;
        lastPacketTime = millis();
        packetCount++;
        
        printSerialData();
        Serial.println();
        printJSON();
        updateDisplay();
        
        Serial.println("✓ OK");
    } else {
        errorCount++;
        Serial.printf("✗ Parse failed (Errors: %lu)\n", errorCount);
    }
    
    Serial.println("═══════════════════════════════════════════════════");
    Serial.println();
    
    digitalWrite(LED_PIN, LOW);
}

// ═══════════════════════════════════════════════════════════
// STATISTICS
// ═══════════════════════════════════════════════════════════

void printStatistics() {
    unsigned long uptime = (millis() - startTime) / 1000;
    float successRate = 0;
    
    if (packetCount + errorCount > 0) {
        successRate = (100.0 * packetCount) / (packetCount + errorCount);
    }
    
    Serial.println();
    Serial.println("╔════════════════════════════════════════╗");
    Serial.println("║         GATEWAY STATISTICS            ║");
    Serial.println("╠════════════════════════════════════════╣");
    Serial.printf("║ Gateway ID      : %-19d║\n", GATEWAY_ID);
    Serial.printf("║ Gateway Name    : %-19s║\n", GATEWAY_NAME);
    Serial.printf("║ Location        : %-19s║\n", GATEWAY_LOCATION);
    Serial.println("╠════════════════════════════════════════╣");
    Serial.printf("║ Packets Received:  %-19lu║\n", packetCount);
    Serial.printf("║ Parse Errors    : %-19lu║\n", errorCount);
    Serial.printf("║ Success Rate    : %-18.1f%%║\n", successRate);
    Serial.printf("║ Uptime          : %-19lus║\n", uptime);
    Serial.println("╚════════════════════════════════════════╝");
    Serial.println();
}

// ═══════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(1000);
    startTime = millis();
    
    Serial.println();
    Serial.println("╔════════════════════════════════════════════════════╗");
    Serial.printf("║     LoRa32 Gateway #%-2d                           ║\n", GATEWAY_ID);
    Serial.printf("║     Name: %-40s║\n", GATEWAY_NAME);
    Serial.printf("║     Location: %-36s║\n", GATEWAY_LOCATION);
    Serial.println("╠════════════════════════════════════════════════════╣");
    Serial.println("║     Simple Version - No WiFi Required             ║");
    Serial.println("╚════════════════════════════════════════════════════╝");
    Serial.println();
    
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    
    Serial.print("[LED] Test...  ");
    for (int i = 0; i < 3; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(100);
        digitalWrite(LED_PIN, LOW);
        delay(100);
    }
    Serial.println("OK");
    
    Serial.print("[OLED] Init... ");
    Wire.begin(OLED_SDA, OLED_SCL);
    Wire.setClock(400000);
    
    if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        oledAvailable = true;
        Serial. println("OK");
        showSplashScreen();
        delay(2000);
    } else {
        oledAvailable = false;
        Serial.println("FAILED");
    }
    
    Serial.print("[LoRa] Init... ");
    SPI.begin(SCK_LORA, MISO_LORA, MOSI_LORA, SS_LORA);
    LoRa.setPins(SS_LORA, RST_LORA, DIO0_LORA);
    
    if (!LoRa.begin(LORA_BAND)) {
        Serial.println("FAILED!");
        if (oledAvailable) {
            showError("LoRa Init Failed!");
        }
        while (1) {
            digitalWrite(LED_PIN, HIGH);
            delay(200);
            digitalWrite(LED_PIN, LOW);
            delay(200);
        }
    }
    
    Serial.println("OK");
    
    LoRa.setSpreadingFactor(10);
    LoRa.setSignalBandwidth(125E3);
    LoRa.setCodingRate4(5);
    LoRa.setSyncWord(0x12);
    LoRa.enableCrc();
    LoRa.setTxPower(20);
    
    Serial.println("[LoRa] Config:  SF10, BW125, SW: 0x12");
    Serial.printf("✓ Gateway #%d ready on 923 MHz\n\n", GATEWAY_ID);
    
    if (oledAvailable) {
        display.clearDisplay();
        display.setTextSize(1);
        display.setCursor(0, 5);
        display.printf("Gateway #%d", GATEWAY_ID);
        display.setCursor(0, 20);
        display.println(GATEWAY_NAME);
        display.setCursor(0, 35);
        display.println("READY");
        display.setCursor(0, 50);
        display.println("Listening...");
        display.display();
    }
    
    for (int i = 0; i < 5; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(50);
        digitalWrite(LED_PIN, LOW);
        delay(50);
    }
}

// ═══════════════════════════════════════════════════════════
// LOOP
// ═══════════════════════════════════════════════════════════

void loop() {
    int packetSize = LoRa.parsePacket();
    if (packetSize) {
        onLoRaReceive(packetSize);
    }
    
    static unsigned long lastDisplayUpdate = 0;
    if (oledAvailable && millis() - lastDisplayUpdate > 2000) {
        updateDisplay();
        lastDisplayUpdate = millis();
    }
    
    if (systemReady && (millis() - lastPacketTime > 60000)) {
        static unsigned long lastWarning = 0;
        if (millis() - lastWarning > 30000) {
            Serial.printf("⚠ [GW#%d] No data for 60+ sec\n", GATEWAY_ID);
            lastWarning = millis();
        }
    }
    
    static unsigned long lastStats = 0;
    if (millis() - lastStats > 300000) {
        printStatistics();
        lastStats = millis();
    }
    
    vTaskDelay(10 / portTICK_PERIOD_MS);
}

/*
 * ═══════════════════════════════════════════════════════════
 *  END OF CODE
 * ═══════════════════════════════════════════════════════════
 * 
 *  SETUP INSTRUCTIONS:
 *  
 *  1. Edit lines 28-30: 
 *     - GATEWAY_ID: 1, 2, 3, or 4
 *     - GATEWAY_NAME: Your gateway name
 *     - GATEWAY_LOCATION: Your location
 *  
 *  2. Upload to LoRa32 v2.1
 *  
 *  3. Repeat for other gateways (change ID each time)
 *  
 *  4. No WiFi configuration needed! 
 *  
 * ═══════════════════════════════════════════════════════════
 */