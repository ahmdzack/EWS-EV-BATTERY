/*
 * ═══════════════════════════════════════════════════════════
 * T-Beam v1.1 Node - MERGED BINARY VERSION
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

// 1. DEFINISI STRUCT BINARY (WAJIB SAMA DENGAN GATEWAY)
struct __attribute__((packed)) PayloadBMS {
    uint8_t nodeId = 1;          // ID Bus ini
    uint16_t cellVoltages[24];   // 24 sel (dalam milivolt)
    int32_t current;             // Arus (mA)
    uint8_t soc;                 // %
    float lat;
    float lon;
    uint16_t bmsTemp;            // Suhu
};

PayloadBMS dataToSend;

// Konfigurasi Hardware (Sesuai file asli Anda)
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

// Variabel Kontrol BLE (Ganti MAC sesuai milik Anda)
std::string targetMAC = "AA:BB:CC:DD:EE:FF"; 
bool deviceConnected = false;

// --- Callback BLE (Disederhanakan untuk contoh) ---
class MyClientCallback : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pClient) { deviceConnected = true; }
    void onDisconnect(NimBLEClient* pClient) { deviceConnected = false; }
};

void setup() {
    Serial.begin(115200);
    SerialGPS.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
    
    // Setup OLED
    Wire.begin(OLED_SDA, OLED_SCL);
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("OLED Fail");

    // Setup LoRa (SF7, BW250)
    SPI.begin(SCK, MISO, MOSI, SS);
    LoRa.setPins(SS, RST, DIO0);
    if (!LoRa.begin(915E6)) {
        Serial.println("LoRa Error!");
        while(1);
    }
    LoRa.setSpreadingFactor(7);
    LoRa.setSignalBandwidth(250E3);
    LoRa.enableCrc();

    // Setup BLE
    NimBLEDevice::init("");
    NimBLEClient* pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(new MyClientCallback());
    
    Serial.println("Node TX Merged & Ready");
}

void loop() {
    // 1. Update GPS
    while (SerialGPS.available() > 0) gps.encode(SerialGPS.read());
    if (gps.location.isValid()) {
        dataToSend.lat = gps.location.lat();
        dataToSend.lon = gps.location.lng();
    }

    // 2. Simulasi Data BMS (Ganti bagian ini dengan parsing BLE asli Anda)
    for(int i=0; i<24; i++) {
        dataToSend.cellVoltages[i] = 3200 + random(0, 150); // 3.2V - 3.35V
    }
    dataToSend.soc = 85;
    dataToSend.current = -4500; // -4.5A

    // 3. Kirim Data via LoRa (BINARY)
    sendLoRaBinary();

    // 4. Update OLED
    updateDisplay();

    delay(3000); // Kirim tiap 3 detik
}

void sendLoRaBinary() {
    Serial.print("Kirim paket binary... ");
    LoRa.beginPacket();
    LoRa.write((uint8_t*)&dataToSend, sizeof(dataToSend));
    LoRa.endPacket();
    Serial.println("Sukses.");
}

void updateDisplay() {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0,0);
    display.printf("NODE TX - BUS %d\n", dataToSend.nodeId);
    display.printf("SOC: %d%%\n", dataToSend.soc);
    display.printf("GPS: %s\n", gps.location.isValid() ? "FIXED" : "SEARCHING");
    display.printf("RSSI: %d\n", LoRa.packetRssi());
    display.display();
}