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
#define OLED_RESET    -1 // Delar reset med ESP32 om(-1 används)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- WIFI OCH THINGSPEAK INSTÄLLNINGAR ---
//const char* ssid = "DIN_WIFI_NAMN";           
//const char* password = "DIN_WIFI_LOSENORD";   
//const char* thingspeakApiKey = "DIN_API_KEY"; 
const char* server = "http://api.thingspeak.com/update";

const int MOSFET_PIN = 6;
Adafruit_INA3221 ina3221;

// --- INSTÄLLNINGAR FÖR NiMH (1 cell) ---
const float CUTOFF_CURRENT_MA = 476.0; // Brytström vid 1.0V (2.1 ohm totalt)
const int REQUIRED_LOW_COUNT = 5;      // Kräver 5 mätningar i rad under gränsen
const float VOLTAGE_CALIBRATION = 1.128; // Kalibreringsfaktor för spänning

float totalMah = 0.0;
int lowCurrentCounter = 0;
unsigned long lastMillis = 0;
unsigned long lastThingSpeakMillis = 0;
const unsigned long thingspeakInterval = 30000; // 30 sekunder
bool testRunning = true;

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }

  // 1. Initiera I2C (SDA = 8, SCL = 7)
  Wire.begin(8, 7);

  // 2. Initiera OLED-display (adress 0x3C är vanligast)
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 misslyckades att starta!"));
    while(1);
  }
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Startar NiMH-testare...");
  display.display();
  delay(1000);

  // 3. Anslut till Wi-Fi
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Ansluter till WiFi...");
  display.display();
  
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("WiFi Anslutet!");
  display.display();
  delay(1000);

  // 4. Slå på MOSFET för att starta urladdningen
  pinMode(MOSFET_PIN, OUTPUT);
  digitalWrite(MOSFET_PIN, HIGH);
  delay(50);

  // 5. Initiera INA3221
  if (!ina3221.begin(0x40, &Wire)) {
    Serial.println("Kunde inte hitta INA3221!");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Fel: INA3221 hittas ej!");
    display.display();
    while (1) { delay(10); }
  }

  lastMillis = millis();
  lastThingSpeakMillis = millis();
}

void loop() {
  unsigned long currentMillis = millis();

  if (!testRunning) {
    digitalWrite(MOSFET_PIN, LOW);
    
    // Visa slutresultat permanent på displayen
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println(F("=== TEST KLART ==="));
    display.setCursor(0, 20);
    display.print(F("Kapacitet: "));
    display.print(totalMah, 1);
    display.println(F(" mAh"));
    display.setCursor(0, 45);
    display.println(F("Batteri natt 1.0V"));
    display.display();
    
    delay(5000);
    return;
  }

  float elapsedHours = (currentMillis - lastMillis) / 3600000.0;
  lastMillis = currentMillis;

  // 1. Läs av spänningsfallet över shuntmotståndet
  float shunt_V = ina3221.getShuntVoltage(0);
  
  // 2. Beräkna ström i mA (R_shunt = 0.1 ohm)
  float current_mA = (shunt_V * 1000.0) / 0.1;

  // 3. Ackumulera mAh
  if (current_mA > 5.0) {
    totalMah += current_mA * elapsedHours;
  }

  // 4. Beräkna och kalibrera spänningen
  float baseVoltage = (current_mA / 1000.0) * 2.1;
  float estimatedVoltage = baseVoltage * VOLTAGE_CALIBRATION;

  // 5. Avbrottslogik (debounce)
  if (current_mA <= CUTOFF_CURRENT_MA && current_mA > 10.0) {
    lowCurrentCounter++;
    
    if (lowCurrentCounter >= REQUIRED_LOW_COUNT) {
      digitalWrite(MOSFET_PIN, LOW);
      testRunning = false;
      sendToThingSpeak(estimatedVoltage, current_mA, totalMah);
    }
  } else {
    if (lowCurrentCounter > 0) {
      lowCurrentCounter = 0;
    }

    // --- UPPDATERA OLED DISPLAY ---
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print(F("NiMH Test (1 Cell)"));

    display.setCursor(0, 16);
    display.print(F("V: "));
    display.print(estimatedVoltage, 3);
    display.print(F(" V"));

    display.setCursor(0, 30);
    display.print(F("I: "));
    display.print(current_mA, 1);
    display.print(F(" mA"));

    display.setCursor(0, 46);
    display.print(F("Cap: "));
    display.print(totalMah, 1);
    display.print(F(" mAh"));
    
    display.display();

    // Skicka till ThingSpeak var 30:e sekund
    if (currentMillis - lastThingSpeakMillis >= thingspeakInterval) {
      lastThingSpeakMillis = currentMillis;
      sendToThingSpeak(estimatedVoltage, current_mA, totalMah);
    }
  }

  delay(1000);
}

void sendToThingSpeak(float voltage, float current, float mah) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    
    String url = String(server) + "?api_key=" + thingspeakApiKey + 
                 "&field1=" + String(voltage, 2) + 
                 "&field2=" + String(current, 1) + 
                 "&field3=" + String(mah, 1);

    http.begin(url);
    int httpResponseCode = http.GET();
    http.end();
  }
}
