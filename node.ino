/*
 * ═══════════════════════════════════════════════════════════
 *  T-Beam v1.1 Node - Complete System
 *  Version: 3.1 FINAL (GPS Fixed)
 *  
 *  Hardware:  TTGO T-Beam v1.1
 *  
 *  Features:
 *  - BLE Client for JK BMS (read voltage, current, SOC, capacity)
 *  - GPS with improved fix retention (TinyGPS++)
 *  - Battery voltage & percentage monitoring (ADC + IP5306)
 *  - OLED display with all status
 *  - LoRa transmission every 3 seconds
 *  - GPS diagnostics & debugging
 * ═══════════════════════════════════════════════════════════
 */

#include <NimBLEDevice.h>
#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <TinyGPS++.h>
#include <HardwareSerial.h>

// ═══════════════════════════════════════════════════════════
// HARDWARE CONFIGURATION
// ═══════════════════════════════════════════════════════════

// OLED Display
#define OLED_SDA    21
#define OLED_SCL    22
#define OLED_ADDR   0x3C

// LED Indicator
#define LED_PIN     2 

// GPS Serial (T-Beam uses Serial1)
#define GPS_RX      34
#define GPS_TX      12

// LoRa Pins
#define SCK_LORA    5
#define MISO_LORA   19
#define MOSI_LORA   27
#define SS_LORA     18
#define RST_LORA    14
#define DIO0_LORA   26
#define LORA_BAND   923E6 

// Battery Monitoring
#define BATTERY_PIN 35       
#define IP5306_ADDR 0x75     

// ═══════════════════════════════════════════════════════════
// BLE CONFIGURATION (JK BMS)
// ═══════════════════════════════════════════════════════════

// ⚠️ EDIT THIS:  Change to your BMS MAC address
static std::string targetMAC = "c8:47:80:2e:a2:c0";

static NimBLEUUID serviceUUID("ffe0");
static NimBLEUUID charUUID("ffe1");

// BMS Commands
uint8_t CMD_WAKEUP[20]   = { 0xAA, 0x55, 0x90, 0xEB, 0x96, 0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0 };
uint8_t CMD_GET_DATA[20] = { 0xAA, 0x55, 0x90, 0xEB, 0x97, 0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0 };

// ═══════════════════════════════════════════════════════════
// GLOBAL OBJECTS
// ═══════════════════════════════════════════════════════════

Adafruit_SSD1306 display(128, 64, &Wire, -1);
HardwareSerial gpsSerial(1);
TinyGPSPlus gps;

static NimBLEAdvertisedDevice* myDevice;
static NimBLEClient* pClient = nullptr;
static NimBLERemoteCharacteristic* pWriteChar = nullptr;

// ═══════════════════════════════════════════════════════════
// GLOBAL VARIABLES
// ═══════════════════════════════════════════════════════════

// BLE Status
bool deviceFound = false;
bool connected = false;
bool hasWokenUp = false;

// BMS Data Buffer
uint8_t buffer[4096];
int bufferLen = 0;

// BMS Data
float globalVolt = 0.0;
float globalAmp = 0.0;
float globalSoc = 0.0;
float globalCap = 0.0;
bool cellsRead = false;
bool dataValid = false;

// GPS Data
double gpsLat = 0.0;
double gpsLng = 0.0;
float gpsSpeed = 0.0;
int gpsSats = 0;
bool gpsValid = false;
unsigned long lastGPSValidTime = 0;  // Track when GPS was last valid

// Battery Data (T-Beam itself)
float batteryVoltage = 0.0;
int batteryPercent = 0;
bool batteryCharging = false;

// Timers
unsigned long lastLoRaSend = 0;
const unsigned long loraInterval = 3000; 
unsigned long lastRequest = 0;
unsigned long lastBatteryUpdate = 0;

// ═══════════════════════════════════════════════════════════
// BATTERY MONITORING FUNCTIONS
// ═══════════════════════════════════════════════════════════

float readBatteryVoltage() {
    uint16_t adcValue = analogRead(BATTERY_PIN);
    const float calibration = 1.10;  // Adjust if needed
    float voltage = (adcValue / 4095.0) * 2.0 * 3.3 * calibration;
    return voltage;
}

int calculateBatteryPercent(float voltage) {
    if (voltage >= 4.20) return 100;
    if (voltage >= 4.00) return 75 + (int)((voltage - 4.00) / 0.20 * 25);
    if (voltage >= 3.70) return 50 + (int)((voltage - 3.70) / 0.30 * 25);
    if (voltage >= 3.50) return 25 + (int)((voltage - 3.50) / 0.20 * 25);
    if (voltage >= 3.00) return (int)((voltage - 3.00) / 0.50 * 25);
    return 0;
}

bool readIP5306Battery() {
    Wire.beginTransmission(IP5306_ADDR);
    Wire.write(0x78);
    
    if (Wire.endTransmission(false) == 0) {
        Wire.requestFrom((uint8_t)IP5306_ADDR, (uint8_t)1);
        if (Wire.available()) {
            uint8_t data = Wire.read();
            int level = data & 0x03;
            
            switch(level) {
                case 0b00: batteryPercent = 12; break;
                case 0b01: batteryPercent = 37; break;
                case 0b10: batteryPercent = 62; break;
                case 0b11: batteryPercent = 87; break;
            }
            
            batteryCharging = (data & 0x08) != 0;
            return true;
        }
    }
    return false;
}

void updateBatteryStatus() {
    batteryVoltage = readBatteryVoltage();
    
    if (! readIP5306Battery()) {
        batteryPercent = calculateBatteryPercent(batteryVoltage);
        batteryCharging = false;
    }
}

// ═══════════════════════════════════════════════════════════
// BMS HELPER FUNCTIONS
// ═══════════════════════════════════════════════════════════

void hitungChecksum(uint8_t* cmd) {
    uint8_t sum = 0;
    for(int i = 0; i < 19; i++) sum += cmd[i];
    cmd[19] = sum;
}

void decodeData() {
    bool foundCells = false;
    int cellPacketIndex = -1;

    // 1. SEARCH FOR CELL VOLTAGES
    for (int i = 0; i < bufferLen - 60; i++) {
        if (buffer[i] == 0x55 && buffer[i+1] == 0xAA && 
            buffer[i+2] == 0xEB && buffer[i+3] == 0x90 && buffer[i+4] == 0x02) {
            
            cellPacketIndex = i;
            float sumVoltage = 0;
            int startCell = i + 6; 
            
            for (int c = 0; c < 24; c++) {
                int idx = startCell + (c * 2);
                if (idx > bufferLen - 2) break;
                
                uint16_t cellRaw = buffer[idx] | (buffer[idx+1] << 8);
                if(cellRaw == 0) break; 
                
                sumVoltage += (cellRaw * 0.001);
            }
            
            if (sumVoltage > 10.0) {
                globalVolt = sumVoltage; 
                cellsRead = true;
                foundCells = true;
            }
        }
    }

    // 2. SEARCH FOR CURRENT
    for (int i = 0; i < bufferLen - 20; i++) {
        if (buffer[i] == 0xAA && buffer[i+1] == 0x55 && 
            buffer[i+2] == 0x90 && buffer[i+3] == 0xEB) {
            
            int idxAmp = i + 8;
            int32_t rawAmp = buffer[idxAmp] | (buffer[idxAmp+1] << 8) | 
                             (buffer[idxAmp+2] << 16) | (buffer[idxAmp+3] << 24);
            
            globalAmp = rawAmp * 0.1; 
            if (rawAmp == 0) globalAmp = 0.00;
        }
    }

    // 3. SEARCH FOR SOC & CAPACITY
    for (int i = 4; i < bufferLen - 4; i++) {
        if (buffer[i] == 0xA0 && buffer[i+1] == 0x86 && 
            buffer[i+2] == 0x01 && buffer[i+3] == 0x00) {
            
            int idxRem = i - 4;
            uint32_t remCap = buffer[idxRem] | (buffer[idxRem+1] << 8) | 
                              (buffer[idxRem+2] << 16) | (buffer[idxRem+3] << 24);
            
            globalCap = remCap * 0.001; 
            globalSoc = (globalCap / 100.0) * 100.0;
            if (globalSoc > 100.0) globalSoc = 100.0;
        }
    }

    if (cellsRead) {
        dataValid = true;
        
        Serial.printf("\n=== BMS DATA ===\n");
        Serial.printf("Voltage :  %.2fV\n", globalVolt);
        Serial.printf("Current :  %.2fA\n", globalAmp);
        Serial.printf("SOC     : %.1f%%\n", globalSoc);
        Serial.printf("Capacity: %.2fAh\n", globalCap);
        
        updateDisplay();
    }

    // Buffer management
    if (bufferLen > 2000) { 
        bufferLen = 0; 
    } else if (foundCells) {
        bufferLen = 0; 
    }
}

// ═══════════════════════════════════════════════════════════
// GPS FUNCTIONS - IMPROVED
// ═══════════════════════════════════════════════════════════

void processGPS() {
    // CRITICAL: Read ALL available GPS data
    while (gpsSerial.available() > 0) {
        char c = gpsSerial.read();
        
        // Uncomment to see raw GPS data: 
        // Serial.print(c);
        
        if (gps.encode(c)) {
            // GPS sentence successfully parsed
            
            if (gps.location.isValid()) {
                gpsLat = gps.location.lat();
                gpsLng = gps.location.lng();
                gpsSpeed = gps.speed.kmph();
                gpsSats = gps.satellites.value();
                gpsValid = true;
                lastGPSValidTime = millis();
                
                // Print GPS update every 10 seconds
                static unsigned long lastGPSPrint = 0;
                if (millis() - lastGPSPrint > 10000) {
                    Serial.println("\n═══ GPS UPDATE ═══");
                    Serial.printf("Latitude  : %. 6f\n", gpsLat);
                    Serial.printf("Longitude : %.6f\n", gpsLng);
                    Serial.printf("Speed     :  %.1f km/h\n", gpsSpeed);
                    Serial.printf("Satellites:  %d\n", gpsSats);
                    Serial.printf("HDOP      : %.2f\n", gps.hdop. hdop());
                    Serial. printf("Age       : %lu ms\n", gps.location.age());
                    Serial. println("════════════════════");
                    lastGPSPrint = millis();
                }
            }
        }
    }
    
    // ✅ FIX:  Improved GPS validity check
    // Don't immediately invalidate GPS if we temporarily lose signal
    if (gps.location.isValid() && gps.location.age() < 5000) {
        // Fresh data (less than 5 seconds old)
        gpsValid = true;
        lastGPSValidTime = millis();
    } else {
        // Only mark invalid after 30 seconds without update
        if (millis() - lastGPSValidTime > 30000) {
            gpsValid = false;
        }
        // Otherwise keep last known valid state
    }
    
    // Update satellite count even if not valid
    if (gps.satellites.isValid()) {
        gpsSats = gps. satellites.value();
    }
    
    // GPS diagnostic (every 30 seconds)
    static unsigned long lastDiag = 0;
    if (millis() - lastDiag > 30000) {
        Serial.println("\n--- GPS Diagnostic ---");
        Serial.printf("Valid     : %s\n", gpsValid ? "YES" : "NO");
        Serial.printf("Satellites: %d\n", gpsSats);
        Serial.printf("HDOP      : %.2f\n", gps.hdop. hdop());
        Serial.printf("Chars     : %lu\n", gps.charsProcessed());
        Serial.printf("Sentences : Good=%lu Failed=%lu\n", 
                      gps.passedChecksum(), gps.failedChecksum());
        Serial.printf("Last Valid: %lu sec ago\n", (millis() - lastGPSValidTime) / 1000);
        
        if (gps.charsProcessed() < 10) {
            Serial. println("⚠ WARNING: GPS not receiving data!");
            Serial.println("Check GPS module connection.");
        }
        
        if (gpsSats < 4 && gps.satellites.isValid()) {
            Serial.println("⚠ WARNING: Too few satellites!");
            Serial.println("Move to open sky area.");
        }
        
        Serial.println("----------------------\n");
        lastDiag = millis();
    }
}

// ═══════════════════════════════════════════════════════════
// DISPLAY FUNCTION
// ═══════════════════════════════════════════════════════════

void updateDisplay() {
    display.clearDisplay();
    display.setTextColor(WHITE);
    
    // ═══ LINE 1: BLE Status & Node Battery ═══
    display.setTextSize(1);
    display.setCursor(0, 0);
    if(connected) display.print("BLE: OK");
    else display.print("BLE: --");
    
    // Node battery
    display.setCursor(60, 0);
    if(batteryCharging) {
        display.print("⚡");
    } else {
        if(batteryPercent > 75) display.print("█");
        else if(batteryPercent > 50) display.print("▓");
        else if(batteryPercent > 25) display.print("▒");
        else display.print("░");
    }
    display.printf("%d%%", batteryPercent);
    
    // GPS status (detailed)
    display.setCursor(100, 0);
    if(gpsValid && gpsSats >= 4) {
        display.printf("S%d", gpsSats);  // Good fix (uppercase)
    } else if(gpsSats > 0) {
        display.printf("s%d", gpsSats);  // Searching (lowercase)
    } else {
        display.print("--");             // No signal
    }
    
    // ═══ LINE 2-3: BMS Voltage & SOC ═══
    display. setTextSize(2);
    display.setCursor(0, 14);
    display.printf("%. 1fV", globalVolt);
    
    display.setCursor(75, 14);
    display.printf("%.0f%%", globalSoc);
    
    // ═══ LINE 4: BMS Current ═══
    display.setCursor(0, 32);
    if(globalAmp > 0.1) {
        display.printf("+%.1fA", globalAmp);
    } else if(globalAmp < -0.1) {
        display.printf("%. 1fA", globalAmp);
    } else {
        display.print("0.0A");
    }
    
    // ═══ LINE 5: GPS Latitude ═══
    display.setTextSize(1);
    display.setCursor(0, 50);
    if(gpsValid) {
        display.printf("%. 5f", gpsLat);
    } else if(gpsSats > 0) {
        display.printf("Searching %ds", gpsSats);
    } else {
        display.print("No GPS signal");
    }
    
    // ═══ LINE 6: GPS Longitude / Speed or Battery Info ═══
    display.setCursor(0, 58);
    if(gpsValid && gpsSpeed > 1.0) {  // ✅ FIX:  Hapus spasi di "1. 0"
        display.printf("%.5f %. 0fkm/h", gpsLng, gpsSpeed);
    } else if(gpsValid) {
        display.printf("%. 5f", gpsLng);
    } else {
        display.printf("Bat: %. 2fV Cap: %.1fAh", batteryVoltage, globalCap);
    }
    
    display.display();
}

// ═══════════════════════════════════════════════════════════
// LORA TRANSMISSION
// ═══════════════════════════════════════════════════════════

void sendLoRaData() {
    if (!dataValid) {
        Serial.println("[LoRa] SKIP - No BMS data");
        return;
    }

    // Format:  BMS,Volt,Amp,Soc,Cap,Lat,Lng,Speed,Sats,BatV,BatPct,Charging
    String pkt = "BMS," + 
                 String(globalVolt, 2) + "," + 
                 String(globalAmp, 2) + "," + 
                 String(globalSoc, 1) + "," + 
                 String(globalCap, 2) + ",";
    
    // GPS data (send even if not valid, for debugging)
    if(gpsValid) {
        pkt += String(gpsLat, 6) + "," + 
               String(gpsLng, 6) + "," + 
               String(gpsSpeed, 1) + "," + 
               String(gpsSats);
    } else {
        // Send 0,0 but include satellite count
        pkt += "0,0,0," + String(gpsSats);
    }
    
    // Node battery data
    pkt += "," + String(batteryVoltage, 2) + "," + 
           String(batteryPercent) + "," + 
           String(batteryCharging ? 1 :  0);
    
    Serial.print("[LoRa] TX: ");
    Serial.println(pkt);
    
    LoRa.beginPacket();
    LoRa.print(pkt);
    int success = LoRa.endPacket();
    
    if (success) {
        Serial.println("[LoRa] SUCCESS");
        
        // Print summary
        Serial.printf("  BMS:  %. 1fV %.1fA %. 0f%%\n", globalVolt, globalAmp, globalSoc);
        if (gpsValid) {
            Serial.printf("  GPS: %.6f,%.6f (%d sats) ✓\n", gpsLat, gpsLng, gpsSats);
        } else {
            Serial.printf("  GPS:  Searching...  (%d sats)\n", gpsSats);
        }
        Serial.printf("  Bat: %. 2fV %d%%", batteryVoltage, batteryPercent);
        if (batteryCharging) Serial.print(" ⚡");
        Serial.println();
        
        digitalWrite(LED_PIN, HIGH);
        delay(50);
        digitalWrite(LED_PIN, LOW);
    } else {
        Serial.println("[LoRa] FAILED!");
    }
}

// ═══════════════════════════════════════════════════════════
// BLE CALLBACKS
// ═══════════════════════════════════════════════════════════

void notifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    digitalWrite(LED_PIN, HIGH);
    
    if (bufferLen + length < 4096) {
        memcpy(&buffer[bufferLen], pData, length);
        bufferLen += length;
        if (bufferLen > 150) decodeData();
    }
    
    digitalWrite(LED_PIN, LOW);
}

class MyAdvertisedDeviceCallbacks:  public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
        String addrStr = advertisedDevice->getAddress().toString().c_str();
        String targetStr = targetMAC.c_str();
        addrStr.toLowerCase();
        targetStr.toLowerCase();

        if (addrStr == String(targetMAC. c_str())) {
            Serial.println(">>> BMS FOUND!  <<<");
            NimBLEDevice::getScan()->stop(); 
            myDevice = advertisedDevice;
            deviceFound = true;
        }
    }
};

class MyClientCallbacks :  public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pClient) {
        connected = true;
        hasWokenUp = false;
        Serial. println(">>> CONNECTED <<<");
    }
    void onDisconnect(NimBLEClient* pClient) {
        connected = false;
        deviceFound = false;
        dataValid = false;
        Serial. println(">>> DISCONNECTED <<<");
        updateDisplay();
    }
};

bool connectToBMS() {
    if(pClient != nullptr) NimBLEDevice::deleteClient(pClient);
    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(new MyClientCallbacks());
    pClient->setConnectTimeout(10);
    
    if (! pClient->connect(myDevice)) return false;

    NimBLERemoteService* pService = pClient->getService(serviceUUID);
    if (pService == nullptr) return false;
    
    pWriteChar = pService->getCharacteristic(charUUID);
    if (pWriteChar == nullptr) return false;
    
    if(pWriteChar->canNotify()) {
        if(! pWriteChar->subscribe(true, notifyCallback)) return false;
    }
    
    return true;
}

// ═══════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n");
    Serial.println("╔════════════════════════════════════════╗");
    Serial.println("║   T-Beam Node - Complete System       ║");
    Serial.println("║   BMS + GPS + Battery + LoRa          ║");
    Serial.println("║   Version 3.1 (GPS Fixed)             ║");
    Serial.println("╚════════════════════════════════════════╝");
    Serial.println();
    
    // Init LED
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    
    // Init OLED
    Serial.print("[OLED] Init...  ");
    Wire.begin(OLED_SDA, OLED_SCL);
    if(display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("OK");
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(WHITE);
        display.setCursor(0, 0);
        display.println("T-Beam Node v3.1");
        display.println();
        display.println("Initializing...");
        display.display();
    } else {
        Serial.println("FAILED");
    }
    
    // Init GPS
    Serial.print("[GPS] Init... ");
    gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
    Serial.println("OK");
    
    // GPS Warm-up period
    Serial.println("[GPS] Warming up (30s)...");
    display.clearDisplay();
    display.setCursor(0, 10);
    display.println("GPS Warming Up...");
    display.display();
    
    unsigned long gpsWarmup = millis();
    int dotCount = 0;
    
    while (millis() - gpsWarmup < 30000) {
        // Read GPS during warm-up
        while (gpsSerial.available() > 0) {
            gps.encode(gpsSerial. read());
        }
        
        // Progress indicator
        if (millis() - gpsWarmup > (dotCount + 1) * 3000) {
            Serial.print(".");
            if (gps.satellites.value() > 0) {
                Serial.printf(" [%d sats]", gps.satellites. value());
            }
            dotCount++;
            
            // Update display
            display.clearDisplay();
            display.setCursor(0, 10);
            display. println("GPS Warming Up...");
            display.setCursor(0, 25);
            display.printf("Satellites: %d", gps.satellites.value());
            display.setCursor(0, 40);
            display.printf("Time:  %ds", (millis() - gpsWarmup) / 1000);
            display.display();
        }
        
        delay(100);
    }
    
    Serial.println(" Done!");
    
    if (gps.satellites.value() > 0) {
        Serial.printf("[GPS] Found %d satellites\n", gps.satellites.value());
    } else {
        Serial.println("[GPS] ⚠ No satellites yet (continue anyway)");
        Serial.println("[GPS] GPS may need more time outdoor");
    }
    
    // Init Battery ADC
    Serial.print("[Battery] Init... ");
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    pinMode(BATTERY_PIN, INPUT);
    updateBatteryStatus();
    Serial.printf("OK (%. 2fV, %d%%)\n", batteryVoltage, batteryPercent);
    
    // Init LoRa
    Serial.print("[LoRa] Init... ");
    SPI.begin(SCK_LORA, MISO_LORA, MOSI_LORA, SS_LORA);
    LoRa.setPins(SS_LORA, RST_LORA, DIO0_LORA);
    
    if (!LoRa.begin(LORA_BAND)) {
        Serial.println("FAILED!");
    } else {
        Serial.println("OK");
        
        LoRa.setSpreadingFactor(10);
        LoRa.setSignalBandwidth(125E3);
        LoRa.setCodingRate4(5);
        LoRa.setSyncWord(0x12);
        LoRa.enableCrc();
        LoRa.setTxPower(17);
        
        Serial.println("[LoRa] Config:  SF10, BW125, SW: 0x12");
        
        // Test packet
        Serial.println("[LoRa] Sending test.. .");
        LoRa.beginPacket();
        LoRa.print("BMS,50.4,2.5,85.0,85.00,0,0,0,0,4. 15,82,0");
        LoRa.endPacket();
        Serial.println("[LoRa] Test sent!");
    }

    // Prepare BMS commands
    hitungChecksum(CMD_WAKEUP);
    hitungChecksum(CMD_GET_DATA);

    // Init BLE
    Serial.println("[BLE] Scanning for BMS...");
    NimBLEDevice::init("");
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(99);
    pScan->start(0, false);
    
    Serial.println("[READY] Node started\n");
    Serial.println("═══════════════════════════════════════");
    Serial.println("GPS Tips:");
    Serial.println("- Stay outdoor for best signal");
    Serial.println("- Wait 1-3 minutes for first fix");
    Serial.println("- GPS will maintain fix once locked");
    Serial.println("═══════════════════════════════════════\n");
    
    updateDisplay();
}

// ═══════════════════════════════════════════════════════════
// MAIN LOOP
// ═══════════════════════════════════════════════════════════

void loop() {
    // 1. Process GPS (MUST BE FIRST!)
    processGPS();
    
    // 2. BLE Reconnection
    if (deviceFound && !connected) {
        if (! connectToBMS()) {
            Serial.println("[BLE] Retry.. .");
            delay(1000);
        }
    } 
    else if (!deviceFound && ! NimBLEDevice::getScan()->isScanning()) {
         NimBLEDevice::getScan()->start(0, false);
    }

    unsigned long now = millis();

    // 3. BLE Data Request
    if (connected) {
        if (now - lastRequest > 3000) { 
            if (pWriteChar != nullptr) {
                if (!hasWokenUp) {
                    Serial.println("[BLE] TX: Wake Up");
                    pWriteChar->writeValue((uint8_t*)CMD_WAKEUP, sizeof(CMD_WAKEUP), false);
                    hasWokenUp = true;
                } else {
                    pWriteChar->writeValue((uint8_t*)CMD_GET_DATA, sizeof(CMD_GET_DATA), false);
                }
            }
            lastRequest = now;
        }
    }

    // 4. Battery Update (every 5 seconds)
    if (now - lastBatteryUpdate > 5000) {
        updateBatteryStatus();
        lastBatteryUpdate = now;
    }

    // 5. LoRa Transmission
    if (now - lastLoRaSend > loraInterval) {
        sendLoRaData();
        lastLoRaSend = now;
    }
    
    // 6. Watchdog
    vTaskDelay(10 / portTICK_PERIOD_MS);
}

/*
 * ═══════════════════════════════════════════════════════════
 *  END OF CODE
 * ═══════════════════════════════════════════════════════════
 * 
 *  SETUP INSTRUCTIONS:
 *  
 *  1. Install required libraries:
 *     - NimBLE-Arduino
 *     - LoRa by Sandeep Mistry
 *     - Adafruit SSD1306
 *     - Adafruit GFX
 *     - TinyGPSPlus
 *  
 *  2. Edit line 73:  Change targetMAC to your BMS MAC address
 *  
 *  3. Upload to T-Beam v1.1
 *  
 *  4. Open Serial Monitor (115200 baud)
 *  
 *  5. For GPS: 
 *     - Go outdoor (required for GPS signal)
 *     - Wait 1-3 minutes for first GPS fix
 *     - GPS will maintain fix better after initial lock
 *     - Check "GPS Diagnostic" messages every 30 seconds
 *  
 *  6. Troubleshooting GPS:
 *     - If "Chars:  0" → Check GPS wiring (RX/TX pins)
 *     - If "Satellites: 0" → Move to open sky area
 *     - If "HDOP > 5.0" → GPS signal weak, reposition
 *  
 *  DATA FORMAT (LoRa):
 *  BMS,Volt,Amp,SOC,Cap,Lat,Lng,Speed,Sats,BatV,BatPct,Charging
 *  
 *  Example:
 *  BMS,51.20,-3.40,87.5,87.50,-6.200000,106. 816666,45. 5,8,4.15,82,0
 *  
 * ═══════════════════════════════════════════════════════════
 */