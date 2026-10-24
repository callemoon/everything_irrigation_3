#include <Wire.h>
#include <Adafruit_INA3221.h>

const int MOSFET_PIN = 6;
Adafruit_INA3221 ina3221;

// --- INSTÄLLNINGAR FÖR NiMH (1 cell) ---
// Målet är 1.0V slutspänning. 
// Med 2 ohm last + 0.1 ohm shunt = 2.1 ohm totalt.
// 1.0V / 2.1 ohm = ca 476 mA brytström.
const float CUTOFF_CURRENT_MA = 476.0; 

float totalMah = 0.0;
unsigned long lastMillis = 0;
bool testRunning = true;

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }

  // 1. Slå på MOSFET för att starta urladdningen
  pinMode(MOSFET_PIN, OUTPUT);
  digitalWrite(MOSFET_PIN, HIGH);
  delay(50);

  // 2. Initiera I2C
  Wire.begin(8, 9);
  if (!ina3221.begin(0x40, &Wire)) {
    Serial.println("Kunde inte hitta INA3221!");
    while (1) { delay(10); }
  }

  lastMillis = millis();
  Serial.println("\n========================================");
  Serial.println("  NiMH URLADDNINGSTEST (1 cell) STARTAD ");
  Serial.println("========================================");
}

void loop() {
  // Om testet är klart, håll MOSFET avstängd
  if (!testRunning) {
    digitalWrite(MOSFET_PIN, LOW);
    delay(5000);
    return;
  }

  unsigned long currentMillis = millis();
  float elapsedHours = (currentMillis - lastMillis) / 3600000.0; // Tid i timmar
  lastMillis = currentMillis;

  // 1. Läs av spänningsfallet över shuntmotståndet
  float shunt_V = ina3221.getShuntVoltage(0);
  
  // 2. Beräkna ström i mA (R_shunt = 0.1 ohm)
  float current_mA = (shunt_V * 1000.0) / 0.1;

  // 3. Ackumulera mAh
  if (current_mA > 5.0) {
    totalMah += current_mA * elapsedHours;
  }

  // 4. Stäng av när strömmen sjunker till gränsen för 1.0V
  if (current_mA <= CUTOFF_CURRENT_MA && current_mA > 10.0) {
    digitalWrite(MOSFET_PIN, LOW); // Stäng av MOSFET
    testRunning = false;

    Serial.println("\n========================================");
    Serial.println("    TEST KLART - NiMH NÅTT 1.0V!        ");
    Serial.print("   Uppmätt kapacitet: ");
    Serial.print(totalMah, 1);
    Serial.println(" mAh");
    Serial.println("========================================");
  } else {
    // Uppskatta spänningen över batteriet (I * totalt motstånd)
    float estimatedVoltage = (current_mA / 1000.0) * 2.1;

    Serial.print("Ström: ");
    Serial.print(current_mA, 1);
    Serial.print(" mA | Est. Spänning: ");
    Serial.print(estimatedVoltage, 2);
    Serial.print(" V | Kapacitet: ");
    Serial.print(totalMah, 1);
    Serial.println(" mAh");
  }

  delay(1000);
}
