# Dokumentasi Teknis: EWS-EV-BATTERY

Sistem Peringatan Dini (*Early Warning System*) untuk pemantauan baterai kendaraan listrik berbasis ESP32, LoRa, dan protokol komunikasi BMS.

---

## 1. Konsep Struct Binary vs String
Dalam transmisi data nirkabel (khususnya LoRa), efisiensi *payload* sangat krusial. Sistem ini beralih dari pengiriman teks ke **Struct Binary**.

| Karakteristik | Cara Lama (CSV/String) | Cara Baru (Struct Binary) |
| :--- | :--- | :--- |
| **Contoh Data** | `"4.12,4.01,3.99"` | `[0x01, 0x9C, 0x01, 0x91, ...]` |
| **Ukuran** | 14 Byte | 12 Byte (3 float x 4 byte) |
| **Efisiensi** | Rendah (Teks diproses sebagai karakter) | Tinggi (Data mentah/blok memori) |

> **Mengapa ini penting di LoRa?** > Semakin kecil ukuran data, semakin rendah **Time on Air (ToA)**. ToA yang rendah mengurangi risiko tabrakan paket (*packet collision*) dan meningkatkan **Packet Delivery Ratio (PDR)**.

---

## 2. Alur Kerja Utama (Main Flow)
Program berjalan secara *multitasking* (*non-blocking*) dengan urutan operasional sebagai berikut:

1. **Inisialisasi (Setup):** Aktivasi modul OLED, GPS (Serial1), LoRa (SPI), dan NimBLE (Bluetooth Low Energy).
2. **Koneksi BLE:** ESP32 mencari perangkat dengan MAC Address spesifik `c8:47:80:2e:a2:c0`. Setelah ditemukan, sistem melakukan *handshake* dan *subscribe* ke Karakteristik `0xFFE1`.
3. **Trigger Request:** ESP32 mengirim perintah `CMD_GET_DATA` setiap 3 detik ke BMS untuk memicu pengiriman data.
4. **Penerimaan & Decoding:** Data masuk melalui `notifyCallback`, dikumpulkan dalam *buffer*, lalu dibedah melalui fungsi `decodeData()`.
5. **Data Fusion:** Penggabungan variabel dari 3 sumber:
   * **BMS:** Tegangan, Arus, SOC.
   * **GPS:** Lintang (Lat), Bujur (Lng), Jumlah Satelit.
   * **Internal:** Level baterai perangkat T-Beam.
6. **Transmisi:** Data yang telah digabung dikirim via LoRa ke Gateway dan ditampilkan di layar OLED.

---

## 3. Parsing Protokol JK02/JK04 (Sistem TLV)
Fungsi `decodeData()` menggunakan metode **Tag-Length-Value (TLV)**, bukan posisi byte statis. Hal ini membuat pembacaan lebih fleksibel terhadap perubahan *firmware* BMS.

### Identifikasi Header
Sistem mencari Header `4E 57` untuk memastikan validitas awal paket. Byte ke-2 dan ke-3 digunakan untuk menentukan `Payload Length`.

### Daftar Tag Utama
| Tag | Deskripsi | Logika Parsing |
| :--- | :--- | :--- |
| **0x83** | Total Voltage | `(HighByte << 8 | LowByte) * 0.01` |
| **0x84** | Current | Menggunakan mask `0x7FFF` untuk nilai; Bit `0x8000` menentukan arah arus (Charge/Discharge). |
| **0x85** | SOC | Diambil langsung sebagai 1 byte persentase (0-100%). |
| **0x79** | Cell Info | Berisi tegangan per sel. Saat ini dilewati (`p += buffer[p] + 1`) untuk efisiensi memori. |

---

## 4. Validasi Data (Checksum)
Untuk menjamin integritas data dari gangguan sinyal Bluetooth, digunakan **Additive Checksum**:

* **Algoritma:** Menjumlahkan seluruh byte dari header hingga akhir payload ke dalam variabel `uint32_t`.
* **Verifikasi:** Membandingkan hasil hitung lokal dengan 4 byte terakhir yang dikirim oleh BMS.
* **Aksi:** Jika hasil tidak cocok (*mismatch*), data dianggap korup dan dibuang.

---

## 5. Konfigurasi Parameter LoRa
Optimalisasi koneksi dilakukan melalui pengaturan Spreading Factor dan Bandwidth.

### Spreading Factor (SF7)
* **Keunggulan:** Kecepatan transmisi paling tinggi, konsumsi daya rendah.
* **Kekurangan:** Jarak jangkauan lebih pendek dibandingkan SF12.
* **Justifikasi:** Dipilih untuk mendapatkan ToA serendah mungkin pada area kampus/perkotaan.

### Bandwidth (BW 250 kHz)
* **Fungsi:** Menggunakan spektrum yang lebih lebar untuk meningkatkan *Data Rate*.
* **Dampak:** Mempercepat transmisi (ToA rendah) namun sedikit menurunkan sensitivitas penerima.

---

## 6. Indikator Performa Sistem (KPI)
Berikut adalah parameter yang digunakan untuk mengukur kualitas jaringan EWS:

* **RSSI (Received Signal Strength Indicator):** Kekuatan daya sinyal (dBm).  
  *Rentang: -50 dBm (Sangat Kuat) hingga -120 dBm (Sangat Lemah).*
* **SNR (Signal-to-Noise Ratio):** Rasio sinyal terhadap gangguan. Jika SNR > 0, sinyal berada di atas kebisingan lingkungan.
* **ToA (Time on Air):** Durasi paket di udara. ToA rendah = transmisi cepat = hemat baterai = minim tabrakan data.
* **PDR (Packet Delivery Ratio):** Persentase keberhasilan pengiriman paket.
  $$\text{PDR} = \left( \frac{\text{Paket Diterima}}{\text{Paket Dikirim}} \right) \times 100\%$$

---

## 7. Justifikasi Penggunaan Multi-Gateway (3 Gateway)
Penggunaan tiga gateway memberikan keunggulan teknis yang signifikan untuk operasional bus listrik:

1. **Spatial Diversity (Keragaman Spasial):** Menghindari *blank spot* atau efek *shadowing* akibat gedung-gedung tinggi di jalur bus.
2. **Redundansi Data:** Jika Gateway A gagal menerima paket karena interferensi lokal, Gateway B atau C kemungkinan besar tetap dapat menangkapnya, sehingga meningkatkan PDR secara keseluruhan.
3. **Reliabilitas Sistem:** Menjamin ketersediaan data pemantauan setiap saat (*high availability*). Jika satu gateway mengalami kendala daya atau internet, sistem tetap berjalan normal melalui dua gateway lainnya.

## 8. Sistem Redundansi pada Multi-Gateway LoRa
Dalam konteks arsitektur *Early Warning System* (EWS) ini, **sistem redundansi** adalah kemampuan jaringan untuk tetap berfungsi secara optimal meskipun terjadi kegagalan komponen (seperti *gateway* mati) atau gangguan sinyal.

Berikut adalah mekanisme kerja redundansi pada sistem multi-gateway:

* **Redundansi Spasial (*Spatial Redundancy*):**
  Mengingat *shuttle bus* selalu bergerak, sinyal LoRa rentan terhalang oleh gedung atau kontur lingkungan (*shadowing*). Dengan menggunakan 3 *gateway* di lokasi yang berbeda, jika pancaran sinyal ke Gateway 1 terhalang, Gateway 2 atau Gateway 3 kemungkinan besar masih berada di area tangkapan sinyal yang baik untuk menerima paket data tersebut.
* **Peningkatan PDR (*Packet Delivery Ratio*):**
  Transmisi LoRa dari *node* bersifat *broadcast*. Satu paket data yang dipancarkan oleh bus akan diterima oleh ketiga *gateway* secara bersamaan. *Server* di *backend* kemudian akan memproses paket pertama yang tiba dan secara otomatis mengabaikan duplikatnya. Metode ini meminimalisir kemungkinan hilangnya data di udara, sehingga nilai PDR dapat didorong mendekati 100%.
* **Ketahanan Kesalahan (*Fault Tolerance*):**
  Jika salah satu *gateway* mengalami *downtime* (misalnya akibat pemadaman listrik atau hilangnya koneksi internet lokal), sistem pemantauan tidak akan lumpuh. Aliran data dari bus akan langsung di-cover oleh *gateway* yang tersisa. Ketersediaan data yang terus-menerus (*high availability*) ini sangat krusial untuk aspek *safety* baterai EV.

---

## 9. Struktur Data (*Shared Data Structure*)
Agar komunikasi *Struct Binary* via LoRa berhasil diproses, kedua perangkat (**Transmitter/Node** dan **Receiver/Gateway**) wajib mendefinisikan bentuk data yang sama persis. Jika strukturnya berbeda, data yang di-pasing akan rusak (*corrupt*).

Berikut adalah definisi struktur C++ yang digunakan:

```cpp
// Simpan deklarasi ini di bagian atas kode untuk kedua file (Node & Gateway)
struct __attribute__((packed)) PayloadBMS {
    uint8_t nodeId = 1;          // ID Bus (1 Byte)
    uint16_t cellVoltages[24];   // Tegangan 24 Sel dalam milivolt (48 Byte, misal: 3.2V -> 3200)
    int32_t current;             // Arus baterai dalam mA (4 Byte)
    uint8_t soc;                 // State of Charge dalam persentase % (1 Byte)
    float latitude;              // Koordinat GPS Latitude (4 Byte)
    float longitude;             // Koordinat GPS Longitude (4 Byte)
    uint32_t timestamp;          // Waktu pengiriman paket data (4 Byte)
};
