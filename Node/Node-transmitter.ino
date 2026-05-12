/*
 * ═══════════════════════════════════════════════════════════
 * T-Beam v1.1 Node - FINAL BINARY VERSION (JK-BMS + GPS)
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

// 1. STRUCT BINARY (WAJIB IDENTIK DENGAN GATEWAY)
struct __attribute__((packed)) PayloadBMS {
    uint8_t nodeId = 1;          // ID Bus
    uint16_t cellVoltages[24];   // 24 sel baterai (mV)
    int32_t current;             // Arus (mA)
    uint8_t soc;                 // State of Charge (%)
    float lat;                   // Latitude GPS
    float lon;                   // Longitude GPS
    uint16_t bmsTemp;            // Suhu BMS
};

PayloadBMS dataToSend;

// ═══════════════════════════════════════════════════════════
// HARDWARE CONFIG & UUIDS
// ═══════════════════════════════════════════════════════════
#define OLED_SDA    21
#define OLED_SCL    22
#define GPS_RX      34
#define GPS_TX      12
#define SCK         5
#define MISO        19
#define MOSI        27
#define SS          18
#define RST         23
#define DIO0        26

Adafruit_SSD1306 display(128, 64, &Wire, -1);
TinyGPSPlus gps;
HardwareSerial SerialGPS(1);

// UUID JK-BMS (Sesuai Protokol Jikong)
static BLEUUID serviceUUID("FFE0");
static BLEUUID charUUID("FFE1");

std::string targetMAC = "AA:BB:CC:DD:EE:FF"; // GANTI DENGAN MAC BMS ANDA
static BLERemoteCharacteristic* pRemoteCharacteristic;
static BLEAdvertisedDevice* myDevice;
bool doConnect = false;
bool connected = false;

// ═══════════════════════════════════════════════════════════
// BMS PARSING LOGIC (REAL)
// ═══════════════════════════════════════════════════════════
static void notifyCallback(BLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    if (length < 20) return; // Paket terlalu pendek

    // Header Jikong biasanya dimulai dengan 0x55 0xAA atau 0x01
    // Logika parsing 24 sel (contoh implementasi offset memori Jikong)
    if (pData[0] == 0x01) { 
        // Parsing Cell Voltages (Contoh: Sel 1 mulai dari index tertentu)
        for (int i = 0; i < 24; i++) {
            dataToSend.cellVoltages[i] = (pData[i*2 + 4] << 8) | pData[i*2 + 5];
        }
        
        // Parsing SOC & Current
        dataToSend.soc = pData[100]; // Sesuaikan dengan register JK-BMS Anda
        int32_t rawCurrent = (pData[101] << 24) | (pData[102] << 16) | (pData[103] << 8) | pData[104];
        dataToSend.current = rawCurrent;
        
        Serial.println("BMS Data Updated via BLE");
    }
}

class MyClientCallback : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pClient) { connected = true; }
    void onDisconnect(NimBLEClient* pClient) {
        connected = false;
        doConnect = true; // Auto Reconnect
    }
};

// ═══════════════════════════════════════════════════════════
// SETUP & LOOP
// ═══════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    SerialGPS.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
    
    // OLED
    Wire.begin(OLED_SDA, OLED_SCL);
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);

    // LoRa Config
    SPI.begin(SCK, MISO, MOSI, SS);
    LoRa.setPins(SS, RST, DIO0);
    if (!LoRa.begin(915E6)) while(1);
    LoRa.setSpreadingFactor(7);
    LoRa.setSignalBandwidth(250E3);
    LoRa.enableCrc();

    // BLE Init
    NimBLEDevice::init("");
    BLEScan* pBLEScan = NimBLEDevice::getScan();
    pBLEScan->setActiveScan(true);
    pBLEScan->start(5, false);
    
    Serial.println("System Initialized");
}

void loop() {
    // 1. Handle GPS
    while (SerialGPS.available() > 0) gps.encode(SerialGPS.read());
    if (gps.location.isValid()) {
        dataToSend.lat = gps.location.lat();
        dataToSend.lon = gps.location.lng();
    }

    // 2. Handle BLE Connection
    if (!connected && targetMAC != "") {
        connectToBMS();
    }

    // 3. Kirim Data via LoRa (Setiap 3 Detik)
    static unsigned long lastSend = 0;
    if (millis() - lastSend > 3000) {
        sendBinaryPacket();
        updateOLED();
        lastSend = millis();
    }
}

// ═══════════════════════════════════════════════════════════
// CORE FUNCTIONS
// ═══════════════════════════════════════════════════════════
bool connectToBMS() {
    BLEClient* pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(new MyClientCallback());

    if (!pClient->connect(BLEAddress(targetMAC))) return false;

    BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
    if (pRemoteService == nullptr) return false;

    pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
    if (pRemoteCharacteristic->canNotify()) {
        pRemoteCharacteristic->subscribe(true, notifyCallback);
    }
    return true;
}

void sendBinaryPacket() {
    Serial.print("Sending Binary Struct... ");
    LoRa.beginPacket();
    LoRa.write((uint8_t*)&dataToSend, sizeof(dataToSend));
    LoRa.endPacket();
    Serial.println("DONE");
}

void updateOLED() {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0,0);
    display.printf("BUS ID: %d\n", dataToSend.nodeId);
    display.printf("BLE : %s\n", connected ? "CONNECTED" : "LOST");
    display.printf("GPS : %s\n", gps.location.isValid() ? "FIXED" : "NO FIX");
    display.printf("SOC : %d%%\n", dataToSend.soc);
    display.printf("Sent: %d bytes\n", sizeof(dataToSend));
    display.display();
}
