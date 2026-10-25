#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <AudioFileSourceICYStream.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include "wifi_password.h"

// --- STIFTDEFINTIONER ---
#define SDA_PIN 8
#define SCL_PIN 7

#define ENCODER_A  20  // U0RXD
#define ENCODER_B  21  // U0txd
#define ENCODER_SW  10  // Tryckknapp (Pinne 10)

#define I2S_BCLK 0
#define I2S_LRC  1
#define I2S_DOUT 3

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define i2c_Address 0x3C 

Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

struct RadioStation {
  const char* name;
  const char* url;
};

RadioStation stations[] = {
  {"1. Sveriges Radio P1",     "http://live1.sr.se/p1-mp3-96"},
  {"2. Sveriges Radio P2",     "http://live1.sr.se/p2-mp3-96"},
  {"3. Sveriges Radio P3",     "http://live1.sr.se/p3-mp3-96"},
  {"4. P4 Stockholm",          "http://live1.sr.se/p4sth-mp3-96"},
  {"5. P4 Uppland",            "http://live1.sr.se/p4uppl-mp3-96"},
  {"6. P4 Gotland",            "http://live1.sr.se/p4gotl-mp3-96"},
  {"7. SR P4 Plus",            "http://live1.sr.se/p4plus-mp3-96"},
  {"8. SR Din Gata",           "http://live1.sr.se/dingata-mp3-96"}
};

const int totalStations = sizeof(stations) / sizeof(stations[0]);
int currentStationIndex = 0;

// Encoder-variabler
volatile long encoderPosition = 0;
volatile int lastEncoded = 0;
volatile int subSteps = 0; 
long lastProcessedPosition = 0;

// Meny- och volymvariabler
enum Mode { MODE_CHANNEL, MODE_VOLUME };
Mode currentMode = MODE_CHANNEL;
float currentGain = 0.4; // Startvolym (0.0 till 1.0)
unsigned long lastModeChangeTime = 0;
const unsigned long MODE_TIMEOUT = 10000; // 10 sekunder innan den hoppar tillbaks

// Timer för att uppdatera Wi-Fi-signal regelbundet
unsigned long lastRssiUpdateTime = 0;
const unsigned long RSSI_INTERVAL = 5000; // Uppdatera var 5:e sekund
long lastKnownRssi = 0;

// Knappvariabler för säker avläsning
bool lastButtonState = HIGH;
unsigned long lastButtonPress = 0;

AudioFileSourceICYStream *file = NULL;
AudioFileSourceBuffer *buff = NULL;
AudioGeneratorMP3 *mp3 = NULL;
AudioOutputI2S *out = NULL;
const int BUFFER_SIZE = 1536;

// Avbrottsrutin för enkodern
void IRAM_ATTR readEncoder() {
  int A = digitalRead(ENCODER_A);
  int B = digitalRead(ENCODER_B);

  int encoded = (A << 1) | B;
  int sum = (lastEncoded << 2) | encoded;

  if (sum == 0b1101 || sum == 0b0100 || sum == 0b0010 || sum == 0b1011) {
    subSteps++;
    if (subSteps >= 2) {
      encoderPosition++;
      subSteps = 0;
    }
  }
  if (sum == 0b1110 || sum == 0b0111 || sum == 0b0001 || sum == 0b1000) {
    subSteps--;
    if (subSteps <= -2) {
      encoderPosition--;
      subSteps = 0;
    }
  }
  lastEncoded = encoded;
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 20);
  display.println("Ansluter till Wi-Fi...");
  display.display();

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  lastKnownRssi = WiFi.RSSI();
}

void stopAudio() {
  if (mp3) {
    mp3->stop();
    delete mp3;
    mp3 = NULL;
  }
  if (buff) {
    delete buff;
    buff = NULL;
  }
  if (file) {
    delete file;
    file = NULL;
  }
}

void startAudio() {
  stopAudio();
  delay(50);
  
  file = new AudioFileSourceICYStream(stations[currentStationIndex].url);
  buff = new AudioFileSourceBuffer(file, BUFFER_SIZE);
  mp3 = new AudioGeneratorMP3();
  
  if (mp3 && buff && file) {
    mp3->begin(buff, out);
    out->SetGain(currentGain);
  }
}

void updateDisplay() {
  display.clearDisplay();
  
  // 1. Översta raden: VOLUME eller RADIO till vänster, Wi-Fi till höger
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  if (currentMode == MODE_VOLUME) {
    display.setCursor(0, 2);
    display.print("VOLUME");
  } else {
    display.setCursor(0, 2);
    display.print("RADIO");
  }
  
  display.setCursor(76, 2);
  display.print("WiFi:");
  display.print(lastKnownRssi);

  // Skiljelinje
  display.drawLine(0, 14, 128, 14, SH110X_WHITE);

  // 2. Mitten: Visar antingen stationsnamn eller Volym-stapel
  if (currentMode == MODE_VOLUME) {
    display.setCursor(0, 26);
    display.print("Volym: ");
    int volPercent = (int)(currentGain * 100);
    display.print(volPercent);
    display.println("%");

    int barWidth = map(volPercent, 0, 100, 0, 120);
    display.drawRect(4, 40, 120, 10, SH110X_WHITE);
    display.fillRect(6, 42, barWidth - 4, 6, SH110X_WHITE);
  } else {
    display.setCursor(0, 26);
    display.println(stations[currentStationIndex].name);

    // 3. Statusfält längst ner i radioläge
    display.setCursor(0, 52);
    if (WiFi.status() == WL_CONNECTED) {
      display.print("Status: Spelar");
    } else {
      display.print("Status: Ansluter...");
    }
  }

  display.display();
}

void setup() {
  pinMode(ENCODER_A, INPUT_PULLUP);
  pinMode(ENCODER_B, INPUT_PULLUP);
  pinMode(ENCODER_SW, INPUT_PULLUP);

  lastEncoded = (digitalRead(ENCODER_A) << 1) | digitalRead(ENCODER_B);
  attachInterrupt(digitalPinToInterrupt(ENCODER_A), readEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_B), readEncoder, CHANGE);

  Wire.begin(SDA_PIN, SCL_PIN);
  delay(250);
  if (!display.begin(i2c_Address, true)) { 
    while (true); 
  }
  
  display.clearDisplay();
  display.display();

  connectWiFi();

  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  out->SetGain(currentGain);

  startAudio();
  updateDisplay();
}

void loop() {
  // 0. Uppdatera Wi-Fi RSSI regelbundet (var 5:e sekund)
  if (millis() - lastRssiUpdateTime > RSSI_INTERVAL) {
    lastRssiUpdateTime = millis();
    if (WiFi.status() == WL_CONNECTED) {
      long newRssi = WiFi.RSSI();
      if (newRssi != lastKnownRssi) {
        lastKnownRssi = newRssi;
        updateDisplay(); 
      }
    }
  }

  // 1. Säker knappavläsning på pinne 10
  bool currentButtonState = digitalRead(ENCODER_SW);
  if (currentButtonState == LOW && lastButtonState == HIGH) {
    if (millis() - lastButtonPress > 300) { 
      lastButtonPress = millis();
      lastModeChangeTime = millis();
      
      if (currentMode == MODE_CHANNEL) {
        currentMode = MODE_VOLUME;
      } else {
        currentMode = MODE_CHANNEL;
      }
      updateDisplay();
    }
  }
  lastButtonState = currentButtonState;

  if (currentMode == MODE_VOLUME && (millis() - lastModeChangeTime > MODE_TIMEOUT)) {
    currentMode = MODE_CHANNEL;
    updateDisplay();
  }

  // 2. Kolla om enkodern har vridits
  noInterrupts();
  long currentPos = encoderPosition;
  interrupts();

  if (currentPos != lastProcessedPosition) {
    int diff = currentPos - lastProcessedPosition;
    lastProcessedPosition = currentPos;
    lastModeChangeTime = millis(); 

    if (currentMode == MODE_CHANNEL) {
      currentStationIndex = (currentStationIndex + (diff > 0 ? 1 : -1)) % totalStations;
      if (currentStationIndex < 0) {
        currentStationIndex = totalStations - 1;
      }
      startAudio();
    } 
    else if (currentMode == MODE_VOLUME) {
      currentGain += (diff > 0 ? 0.05 : -0.05);
      if (currentGain > 1.0) currentGain = 1.0;
      if (currentGain < 0.0) currentGain = 0.0;
      
      if (out) {
        out->SetGain(currentGain);
      }
    }
    updateDisplay();
  }

  // 3. Wi-Fi kontroll
  if (WiFi.status() != WL_CONNECTED) {
    stopAudio();
    connectWiFi();
    startAudio();
    updateDisplay();
    return;
  }

  // 4. Ljudloop
  if (mp3 && mp3->isRunning()) {
    if (!mp3->loop()) {
      stopAudio();
      delay(500);
      startAudio();
    }
  } else {
    delay(500);
    startAudio();
  }

  vTaskDelay(1); 
}
