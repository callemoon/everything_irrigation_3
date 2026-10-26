#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <WiFi.h>
#include <AudioFileSourceICYStream.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include "config.h"

// --- PIN DEFINITIONS ---
#define ENCODER_A   20  // U0RXD
#define ENCODER_B   21  // U0TXD
#define ENCODER_SW  10  // Push button

#define I2S_DOUT 8
#define I2S_BCLK 6
#define I2S_LRC  7

// SPI Display Pins
#define TFT_SCLK 0
#define TFT_MOSI 1
#define TFT_RST  2
#define TFT_DC   3
#define TFT_CS   4
#define PIN_LED  5

// Färgdefinitioner (RGB565)
#define COLOR_BG      0x0821  // Mörkblå/svart bakgrund
#define COLOR_PANEL   0x18C5  // Mörkgrå panel
#define COLOR_BORDER  0x3218  // Subtil ramfärg
#define COLOR_CYAN    0x07FF  // Stark cyan
#define COLOR_ORANGE  0xFCE0  // Neon orange
#define COLOR_GREEN   0x07E0  // Stark grön
#define COLOR_RED     0xF800  // Röd
#define COLOR_WHITE   0xFFFF  // Vit
#define COLOR_GRAY    0x7BEF  // Grå text

SPIClass spiDisplay(FSPI);
Adafruit_ST7789 tft = Adafruit_ST7789(&spiDisplay, TFT_CS, TFT_DC, TFT_RST);

struct RadioStation {
  const char* name;
  const char* url;
  const char* bitrate;
};

RadioStation stations[] = {
  {"1. SR P1",         "http://live1.sr.se/p1-mp3-96",     "96 kbps"},
  {"2. SR P2",         "http://live1.sr.se/p2-mp3-96",     "96 kbps"},
  {"3. SR P3",         "http://live1.sr.se/p3-mp3-96",     "96 kbps"},
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
  tft.fillScreen(COLOR_BG);
  tft.setTextSize(2);
  tft.setTextColor(COLOR_CYAN);
  tft.setCursor(40, 50);
  tft.println("ESP32 INTERNET RADIO");
  tft.setTextSize(1);
  tft.setTextColor(COLOR_ORANGE);
  tft.setCursor(90, 85);
  tft.println("Ansluter till WiFi...");
  tft.drawFastHLine(30, 110, 260, COLOR_BORDER);
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

void updateDisplay() {
  tft.fillScreen(COLOR_BG);

  // 1. Toppanel / Rubrik (320x170 landskap)
  tft.fillRoundRect(8, 6, 304, 32, 4, COLOR_PANEL);
  tft.drawRoundRect(8, 6, 304, 32, 4, COLOR_CYAN);
  
  tft.setTextSize(2);
  tft.setTextColor(COLOR_CYAN);
  tft.setCursor(16, 14);
  if (currentMode == MODE_VOLUME) {
    tft.print("[ VOLUME MODE ]");
  } else {
    tft.print(" ESP32 INTERNET RADIO");
  }

  // 2. Mittpanel: Nuvarande station / Info
  tft.drawRoundRect(8, 44, 304, 66, 4, COLOR_BORDER);
  
  tft.setTextSize(1);
  tft.setTextColor(COLOR_GRAY);
  tft.setCursor(16, 52);
  tft.print("NOW PLAYING:");

  tft.setTextColor(COLOR_WHITE);
  tft.setTextSize(2);
  tft.setCursor(16, 72);
  tft.println(stations[currentStationIndex].name);

  tft.setTextSize(1);
  tft.setTextColor(COLOR_ORANGE);
  tft.setCursor(220, 52);
  tft.print(stations[currentStationIndex].bitrate);

  // 3. Bottenpanel: Volym och WiFi RSSI
  tft.drawRoundRect(8, 116, 304, 32, 4, COLOR_BORDER);
  
  int volPercent = (int)(currentGain * 100);
  tft.setTextColor(COLOR_GRAY);
  tft.setCursor(16, 126);
  tft.print("Vol:");
  tft.print(volPercent);
  tft.print("%");

  // Volymstapel
  tft.drawRect(75, 125, 120, 14, COLOR_BORDER);
  int barWidth = map(volPercent, 0, 100, 0, 116);
  tft.fillRect(77, 127, barWidth, 10, COLOR_GREEN);

  // RSSI till höger
  tft.setTextColor(COLOR_CYAN);
  tft.setCursor(225, 126);
  tft.print("RSSI:");
  tft.print(lastKnownRssi);
  tft.print("dB");

  // 4. Footer längst ner
  tft.fillRect(0, 154, 320, 16, COLOR_PANEL);
  tft.setTextColor(COLOR_GRAY);
  tft.setTextSize(1);
  tft.setCursor(8, 157);
  tft.print(WiFi.status() == WL_CONNECTED ? "Status: Ansluten" : "Status: Ansluter...");
}

void setup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

  pinMode(ENCODER_A, INPUT_PULLUP);
  pinMode(ENCODER_B, INPUT_PULLUP);
  pinMode(ENCODER_SW, INPUT_PULLUP);

  lastEncoded = (digitalRead(ENCODER_A) << 1) | digitalRead(ENCODER_B);
  attachInterrupt(digitalPinToInterrupt(ENCODER_A), readEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_B), readEncoder, CHANGE);

  // Initiera för 170x320 och vrid till landskap (ändra till 3 om den hamnar upp och ned)
  spiDisplay.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.init(170, 320);
  tft.setSPISpeed(40000000); 
  tft.setRotation(3); 

  connectWiFi();

  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  out->SetGain(currentGain);

  startAudio();
  updateDisplay();
}

void loop() {
  if (millis() - lastRssiUpdateTime > RSSI_INTERVAL) {
    lastRssiUpdateTime = millis();
    if (WiFi.status() == WL_CONNECTED) {
      lastKnownRssi = WiFi.RSSI();
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
