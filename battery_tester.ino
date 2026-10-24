#include <Wire.h>
#include <Adafruit_INA3221.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <HTTPClient.h>

#include "config.h"

// --- OLED DISPLAY INSTÄLLNINGAR ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1 
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- PIN-KONFIGURATION ---
const int MOSFET_PIN_CH1 = 6;
const int MOSFET_PIN_CH2 = 9; 

Adafruit_INA3221 ina3221;

const char* server = "http://api.thingspeak.com/update";

// --- INSTÄLLNINGAR FÖR NiMH (1 cell per kanal) ---
const float CUTOFF_CURRENT_MA = 476.0;   // Brytström vid 1.0V (2.1 ohm totalt)
const int REQUIRED_LOW_COUNT = 5;        // Kräver 5 mätningar i rad under gränsen
const float VOLTAGE_CALIBRATION = 1.128; // Kalibreringsfaktor för spänning

// --- STRUKTUR FÖR KANALDATA ---
struct ChannelData {
  int mosfetPin;
  float totalMah;
  int lowCurrentCounter;
  bool active;
  float voltage;
  float current_mA;
};

ChannelData ch1 = {MOSFET_PIN_CH1, 0.0, 0, true, 0.0, 0.0};
ChannelData ch2 = {MOSFET_PIN_CH2, 0.0, 0, true, 0.0, 0.0};

unsigned long lastMillis = 0;
unsigned long lastThingSpeakMillis = 0;
const unsigned long thingspeakInterval = 30000; // 30 sekunder

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }

  // 1. Initiera I2C (SDA = 8, SCL = 7)
  Wire.begin(8, 7);

  // 2. Initiera OLED-display
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 misslyckades att starta!"));
    while (1);
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("Startar 2-Kanal NiMH..."));
  display.display();
  delay(1000);

  // 3. Anslut till Wi-Fi
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println(F("Ansluter till WiFi..."));
  display.display();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println(F("WiFi Anslutet!"));
  display.display();
  delay(1000);

  // 4. Slå på MOSFETs för båda kanalerna
  pinMode(ch1.mosfetPin, OUTPUT);
  pinMode(ch2.mosfetPin, OUTPUT);
  digitalWrite(ch1.mosfetPin, HIGH);
  digitalWrite(ch2.mosfetPin, HIGH);
  delay(50);

  // 5. Initiera INA3221
  if (!ina3221.begin(0x40, &Wire)) {
    Serial.println(F("Kunde inte hitta INA3221!"));
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(F("Fel: INA3221 ej funnen"));
    display.display();
    while (1) { delay(10); }
  }

  lastMillis = millis();
  lastThingSpeakMillis = millis();
}

void loop() {
  unsigned long currentMillis = millis();
  float elapsedHours = (currentMillis - lastMillis) / 3600000.0;
  lastMillis = currentMillis;

  // --- OM BÅDA KANALERNA ÄR FÄRDIGA ---
  if (!ch1.active && !ch2.active) {
    digitalWrite(ch1.mosfetPin, LOW);
    digitalWrite(ch2.mosfetPin, LOW);

    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println(F("=== BADA KLAR ==="));
    display.setCursor(0, 16);
    display.print(F("CH1: ")); display.print(ch1.totalMah, 1); display.println(F(" mAh"));
    display.setCursor(0, 32);
    display.print(F("CH2: ")); display.print(ch2.totalMah, 1); display.println(F(" mAh"));
    display.setCursor(0, 50);
    display.println(F("Batterier nått 1.0V"));
    display.display();

    delay(2000);
    return;
  }

  // --- PROCESSERA KANAL 1 (INA3221 Kanal 0) ---
  processChannel(0, ch1, elapsedHours);

  // --- PROCESSERA KANAL 2 (INA3221 Kanal 1) ---
  processChannel(1, ch2, elapsedHours);

  // --- UPPDATERA OLED DISPLAY ---
  display.clearDisplay();
  display.setTextSize(1);

  // Kanal 1 Visning
  display.setCursor(0, 0);
  display.print(F("CH1: "));
  if (ch1.active) {
    display.print(ch1.voltage, 2); display.print(F("V "));
    display.print(ch1.current_mA, 0); display.print(F("mA"));
  } else {
    display.print(F("KLAR (")); display.print(ch1.totalMah, 1); display.print(F("mAh)"));
  }

  // Kanal 2 Visning
  display.setCursor(0, 16);
  display.print(F("CH2: "));
  if (ch2.active) {
    display.print(ch2.voltage, 2); display.print(F("V "));
    display.print(ch2.current_mA, 0); display.print(F("mA"));
  } else {
    display.print(F("KLAR (")); display.print(ch2.totalMah, 1); display.print(F("mAh)"));
  }

  // Ackumulerad Kapacitet
  display.setCursor(0, 36);
  display.print(F("C1 Cap: ")); display.print(ch1.totalMah, 1); display.print(F(" mAh"));
  display.setCursor(0, 48);
  display.print(F("C2 Cap: ")); display.print(ch2.totalMah, 1); display.print(F(" mAh"));

  display.display();

  // --- PERIODISK THINGSPEAK-LOGGNING ---
  if (currentMillis - lastThingSpeakMillis >= thingspeakInterval) {
    lastThingSpeakMillis = currentMillis;
    sendToThingSpeak();
  }

  delay(1000);
}

// --- HÄLPERFUNKTION FÖR ATT BERÄKNA CH-DATA ---
void processChannel(int inaChannel, ChannelData &ch, float elapsedHours) {
  if (!ch.active) return;

  float shunt_V = ina3221.getShuntVoltage(inaChannel);
  ch.current_mA = (shunt_V * 1000.0) / 0.1;

  if (ch.current_mA > 5.0) {
    ch.totalMah += ch.current_mA * elapsedHours;
  }

  float baseVoltage = (ch.current_mA / 1000.0) * 2.1;
  ch.voltage = baseVoltage * VOLTAGE_CALIBRATION;

  // Avbrottslogik per kanal
  if (ch.current_mA <= CUTOFF_CURRENT_MA && ch.current_mA > 10.0) {
    ch.lowCurrentCounter++;
    if (ch.lowCurrentCounter >= REQUIRED_LOW_COUNT) {
      digitalWrite(ch.mosfetPin, LOW); // Stäng av just denna kanal
      ch.active = false;
    }
  } else {
    ch.lowCurrentCounter = 0;
  }
}

// --- SEND TO THINGSPEAK (SÄNDER BÅDA KANALERNA) ---
void sendToThingSpeak() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;

    String url = String(server) + "?api_key=" + thingspeakApiKey +
                 "&field1=" + String(ch1.voltage, 2) +
                 "&field2=" + String(ch1.current_mA, 1) +
                 "&field3=" + String(ch1.totalMah, 1) +
                 "&field4=" + String(ch2.voltage, 2) +
                 "&field5=" + String(ch2.current_mA, 1) +
                 "&field6=" + String(ch2.totalMah, 1);

    http.begin(url);
    int httpResponseCode = http.GET();
    http.end();
  }
}
