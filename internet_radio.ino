#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <AudioFileSourceICYStream.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include "config.h"

// --- PIN DEFINITIONS ---
#define SDA_PIN 8
#define SCL_PIN 7

#define ENCODER_A  20  // U0RXD
#define ENCODER_B  21  // U0TXD
#define ENCODER_SW  10 // Push button

#define I2S_DOUT 0
#define I2S_BCLK 1
#define I2S_LRC  2

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1 
#define SCREEN_ADDRESS 0x3C 

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

struct RadioStation {
  const char* name;
  const char* url;
  const char* bitrate;
};

RadioStation stations[] = {
  {"1. SR P1",          "http://live1.sr.se/p1-mp3-96",      "96 kbps"},
  {"2. SR P2",          "http://live1.sr.se/p2-mp3-96",      "96 kbps"},
  {"3. SR P3",          "http://live1.sr.se/p3-mp3-96",      "96 kbps"},
  {"4. P4 Stockholm",   "http://live1.sr.se/p4sth-mp3-96",  "96 kbps"},
  {"5. P4 Uppland",     "http://live1.sr.se/p4uppl-mp3-96", "96 kbps"},
  {"6. P4 Gotland",     "http://live1.sr.se/p4gotl-mp3-96", "96 kbps"},
  {"7. SR P4 Plus",     "http://live1.sr.se/p4plus-mp3-96", "96 kbps"},
  {"8. SR Din Gata",    "http://live1.sr.se/dingata-mp3-96", "96 kbps"}
};

const int totalStations = sizeof(stations) / sizeof(stations[0]);
int currentStationIndex = 0;

// Encoder Variables
volatile long encoderPosition = 0;
volatile int lastEncoded = 0;
volatile int subSteps = 0; 
volatile unsigned long lastInterruptTime = 0;
long lastProcessedPosition = 0;

// UI Modes
enum Mode { MODE_CHANNEL, MODE_VOLUME };
Mode currentMode = MODE_CHANNEL;
float currentGain = 0.1; // Default Volume set to 10%

unsigned long lastModeChangeTime = 0;
const unsigned long MODE_TIMEOUT = 10000; 

// Wi-Fi RSSI polling
unsigned long lastRssiUpdateTime = 0;
const unsigned long RSSI_INTERVAL = 3000; 
long lastKnownRssi = -70;

// Button debouncing
bool lastButtonState = HIGH;
unsigned long lastButtonPress = 0;

AudioFileSourceICYStream *file = NULL;
AudioFileSourceBuffer *buff = NULL;
AudioGeneratorMP3 *mp3 = NULL;
AudioOutputI2S *out = NULL;
const int BUFFER_SIZE = 1536;

void IRAM_ATTR readEncoder() {
  unsigned long interruptTime = millis();
  if (interruptTime - lastInterruptTime < 2) return;
  
  int A = digitalRead(ENCODER_A);
  int B = digitalRead(ENCODER_B);

  int encoded = (A << 1) | B;
  int sum = (lastEncoded << 2) | encoded;

  if (sum == 0b1101 || sum == 0b0100 || sum == 0b0010 || sum == 0b1011) {
    subSteps++;
    if (subSteps >= 4) {
      encoderPosition++;
      subSteps = 0;
    }
    lastInterruptTime = interruptTime;
  }
  if (sum == 0b1110 || sum == 0b0111 || sum == 0b0001 || sum == 0b1000) {
    subSteps--;
    if (subSteps <= -4) {
      encoderPosition--;
      subSteps = 0;
    }
    lastInterruptTime = interruptTime;
  }
  lastEncoded = encoded;
}

void showBootScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(24, 20);
  display.println("ESP32 INTERNET");
  display.setCursor(48, 34);
  display.println("RADIO");

  display.drawFastHLine(20, 48, 88, SSD1306_WHITE);
  display.display();
}

void connectWiFi() {
  showBootScreen();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  lastKnownRssi = WiFi.RSSI();
}

void stopAudio() {
  if (mp3) { mp3->stop(); delete mp3; mp3 = NULL; }
  if (buff) { delete buff; buff = NULL; }
  if (file) { delete file; file = NULL; }
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

int getWiFiBars(long rssi) {
  if (rssi > 0 || rssi < -95) return 1;
  if (rssi >= -55) return 4;
  else if (rssi >= -68) return 3;
  else if (rssi >= -80) return 2;
  return 1;
}

void drawWiFiIcon(int x, int y, long rssi) {
  int bars = getWiFiBars(rssi);

  display.fillRect(x, y, 12, 10, SSD1306_BLACK);

  for (int i = 0; i < bars; i++) {
    int h = 3 + (i * 2);
    display.fillRect(x + (i * 3), y + (9 - h), 2, h, SSD1306_WHITE);
  }
}

void updateDisplay() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  if (currentMode == MODE_VOLUME) {
    display.setCursor(2, 3);
    display.print("[ VOLUME ]");
  } else {
    display.setCursor(2, 3);
    display.print(" ESP32 RADIO");
  }
  
  drawWiFiIcon(104, 3, lastKnownRssi);
  display.drawFastHLine(0, 15, 128, SSD1306_WHITE);

  if (currentMode == MODE_VOLUME) {
    display.setCursor(4, 25);
    display.print("Volume:");
    
    int volPercent = (int)(currentGain * 100);
    display.setCursor(90, 25);
    display.print(volPercent);
    display.print("%");

    display.drawRect(4, 40, 120, 12, SSD1306_WHITE);
    int barWidth = map(volPercent, 0, 100, 0, 114);
    display.fillRect(7, 43, barWidth, 6, SSD1306_WHITE);
  } 
  else {
    display.setCursor(4, 22);
    display.print("Now Playing:");
    
    display.setCursor(4, 36);
    display.println(stations[currentStationIndex].name);

    display.drawFastHLine(0, 52, 128, SSD1306_WHITE);
    display.setCursor(2, 55);
    if (WiFi.status() == WL_CONNECTED) {
      int volPercent = (int)(currentGain * 100);
      display.print("V:");
      display.print(volPercent);
      display.print(" ");
      display.print(lastKnownRssi);
      display.print("dBm");
    } else {
      display.print("Connecting...");
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
  
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    while(true); 
  }
  
  display.clearDisplay();
  display.display();

  connectWiFi();

  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  out->SetGain(currentGain);

  startAudio();
  
  // VIKTIGT: Rita ut skärmbilden direkt när allt har startat!
  updateDisplay();
}

void loop() {
  if (millis() - lastRssiUpdateTime > RSSI_INTERVAL) {
    lastRssiUpdateTime = millis();
    if (WiFi.status() == WL_CONNECTED) {
      long newRssi = WiFi.RSSI();
      int oldBars = getWiFiBars(lastKnownRssi);
      int newBars = getWiFiBars(newRssi);
      
      if (newBars != oldBars || newRssi != lastKnownRssi) {
        lastKnownRssi = newRssi;
        updateDisplay(); 
      }
    }
  }

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

  if (currentMode != MODE_CHANNEL && (millis() - lastModeChangeTime > MODE_TIMEOUT)) {
    currentMode = MODE_CHANNEL;
    updateDisplay();
  }

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

  if (WiFi.status() != WL_CONNECTED) {
    stopAudio();
    connectWiFi();
    startAudio();
    updateDisplay();
    return;
  }

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
