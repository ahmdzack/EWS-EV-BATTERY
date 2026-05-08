/*
 * ═══════════════════════════════════════════════════════════
 * LoRa32 BMS Gateway - MERGED BINARY VERSION
 * ═══════════════════════════════════════════════════════════
 */

#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// 1. DEFINISI STRUCT BINARY (WAJIB SAMA DENGAN NODE)
struct __attribute__((packed)) PayloadBMS {
    uint8_t nodeId;
    uint16_t cellVoltages[24]; 
    int32_t current;           
    uint8_t soc;               
    float lat;
    float lon;
    uint16_t bmsTemp;          
};

PayloadBMS incomingData;

// Konfigurasi Pins LoRa32 v2.1
#define OLED_SDA    21
#define OLED_SCL    22
#define SCK         5
#define MISO        19
#define MOSI        27
#define SS          18
#define RST         23
#define DIO0        26

Adafruit_SSD1306 display(128, 64, &Wire, -1);
int displayPage = 0;

void setup() {
    Serial.begin(115200);
    
    // Setup OLED
    Wire.begin(OLED_SDA, OLED_SCL);
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);

    // Setup LoRa
    SPI.begin(SCK, MISO, MOSI, SS);
    LoRa.setPins(SS, RST, DIO0);
    LoRa.begin(915E6);
    LoRa.setSpreadingFactor(7);
    LoRa.setSignalBandwidth(250E3);
    LoRa.enableCrc();

    Serial.println("Gateway RX Merged & Ready");
}

void loop() {
    int packetSize = LoRa.parsePacket();
    if (packetSize == sizeof(incomingData)) {
        // Bongkar paket biner ke struct
        LoRa.readBytes((uint8_t*)&incomingData, packetSize);
        
        // Print ke Serial (Format untuk Debug/Excel)
        Serial.printf("BUS:%d|SOC:%d|Arus:%.2f\n", 
                      incomingData.nodeId, incomingData.soc, (float)incomingData.current/1000.0);
    }

    // Update OLED dengan sistem Paging (Halaman)
    updateOLED();
    delay(2000); // Ganti halaman tiap 2 detik
}

void updateOLED() {
    display.clearDisplay();
    display.setCursor(0,0);
    display.setTextSize(1);
    display.setTextColor(WHITE);

    if (displayPage == 0) {
        display.println("=== STATUS BUS ===");
        display.printf("Bus ID: %d\n", incomingData.nodeId);
        display.printf("SOC   : %d%%\n", incomingData.soc);
        display.printf("Arus  : %.2f A\n", (float)incomingData.current/1000.0);
        display.printf("RSSI  : %d dBm\n", LoRa.packetRssi());
        displayPage = 1;
    } 
    else if (displayPage == 1) {
        display.println("CELLS 1-12 (V):");
        for(int i=0; i<12; i++) {
            display.printf("%.2f ", (float)incomingData.cellVoltages[i]/1000.0);
            if((i+1)%3 == 0) display.println();
        }
        displayPage = 2;
    } 
    else {
        display.println("CELLS 13-24 (V):");
        for(int i=12; i<24; i++) {
            display.printf("%.2f ", (float)incomingData.cellVoltages[i]/1000.0);
            if((i+1)%3 == 0) display.println();
        }
        displayPage = 0;
    }
    display.display();
}