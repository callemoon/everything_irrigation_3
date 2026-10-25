#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

// Definiera I2C-stift för OLED-skärmen
#define SDA_PIN 8
#define SCL_PIN 7

// Definiera stift för Enkodern
#define ENCODER_A  20  // U0RXD
#define ENCODER_B  21  // U0txd
#define ENCODER_SW  4   // Tryckknapp

// Skärmens storlek
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define i2c_Address 0x3C 

// Initiera skärmen
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Variabler för enkodern (volatile eftersom de ändras i interrupt)
volatile long encoderPosition = 0;
volatile int lastEncoded = 0;

// Knappvariabler
bool lastButtonState = HIGH;
bool buttonPressed = false;

// Proffsig interrupt-rutin för kvadraturavkänning
void IRAM_ATTR readEncoder() {
  int A = digitalRead(ENCODER_A);
  int B = digitalRead(ENCODER_B);

  int encoded = (A << 1) | B;
  int sum = (lastEncoded << 2) | encoded;

  if (sum == 0b1101 || sum == 0b0100 ||
      sum == 0b0010 || sum == 0b1011) {
    encoderPosition++;
  }

  if (sum == 0b1110 || sum == 0b0111 ||
      sum == 0b0001 || sum == 0b1000) {
    encoderPosition--;
  }

  lastEncoded = encoded;
}

void setup() {
  Serial.begin(115200);

  // Ställ in stift
  pinMode(ENCODER_A, INPUT_PULLUP);
  pinMode(ENCODER_B, INPUT_PULLUP);
  pinMode(ENCODER_SW, INPUT_PULLUP);

  // Initiera startläge för enkodern
  lastEncoded = (digitalRead(ENCODER_A) << 1) | digitalRead(ENCODER_B);

  // Koppla interrupts till båda enoderns stift
  attachInterrupt(digitalPinToInterrupt(ENCODER_A), readEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_B), readEncoder, CHANGE);

  // Starta I2C och skärm
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(250);
  if (!display.begin(i2c_Address, true)) { 
    Serial.println("Kunde inte hitta OLED-skärmen!");
    while (true); 
  }

  display.clearDisplay();
  display.display();
  Serial.println("System startat!");
}

void loop() {
  // 1. Läs av tryckknappen med enkel mjukvaruavstörning
  bool currentButtonState = digitalRead(ENCODER_SW);
  if (lastButtonState == HIGH && currentButtonState == LOW) {
    buttonPressed = !buttonPressed; 
    delay(50); 
  }
  lastButtonState = currentButtonState;

  // 2. Hämta enkoderns position säkert (skyddad mot interrupt-krockar)
  noInterrupts();
  long currentPosition = encoderPosition;
  interrupts();

  // 3. Uppdatera OLED-skärmen
  display.clearDisplay();

  // Rubrik
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(10, 5);
  display.println("Encoder (Proffs)");

  // Visa enkodervärdet med stor text
  display.setTextSize(2);
  display.setCursor(10, 20);
  display.print("Val: ");
  display.print(currentPosition);

  // Visa knappstatus längst ner
  display.setTextSize(1);
  display.setCursor(10, 50);
  display.print("Knapp: ");
  display.print(buttonPressed ? "NEDTRYCKT" : "FRI");

  // Rita ram och skicka till skärmen
  display.drawRect(0, 0, 128, 64, SH110X_WHITE);
  display.display();
  
  // Liten fördröjning för att spara på CPU-kraft, men eftersom 
  // enkodern körs på interrupt missar vi inga steg ändå!
  delay(15); 
}
