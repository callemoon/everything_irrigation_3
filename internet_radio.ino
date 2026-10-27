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
#define ENCODER_A    20  // U0RXD
#define ENCODER_B    21  // U0TXD
#define ENCODER_SW   10  // Push button

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
#define COLOR_BG      0x0000  // Svart bakgrund för ren look
#define COLOR_CYAN    0x07FF  // Stark cyan
#define COLOR_ORANGE  0xFCE0  // Neon orange
#define COLOR_GREEN   0x07E0  // Stark grön
#define COLOR_WHITE   0xFFFF  // Vit
#define COLOR_GRAY    0x7BEF  // Grå text
#define COLOR_DIM_GRAY 0x4208 // Mörkare grå för avdelare

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
  {"5. P4 Uppland",    "http://live1.sr.se/p4uppl-mp3-96", "96 kbps"},
  {"6. P4 Gotland",    "http://live1.sr.se/p4gotl-mp3-96", "96 kbps"},
  {"7. SR P4 Plus",    "http://live1.sr.se/p4plus-mp3-96", "96 kbps"},
  {"8. SR Din Gata",   "http://live1.sr.se/dingata-mp3-96", "96 kbps"}
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
float currentGain = 0.1; // Standardvolym 10%

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

// Ritar upp hela gränssnittet (endast vid uppstart och lägesbyte)
void updateDisplay() {
  tft.fillScreen(COLOR_BG);

  // 1. Minimalistisk header (visar läge)
  tft.setTextSize(1);
  if (currentMode == MODE_VOLUME) {
    tft.setTextColor(COLOR_ORANGE);
    tft.setCursor(10, 10);
    tft.print("> JUSTERAR VOLYM <");
  } else {
    tft.setTextColor(COLOR_GRAY);
    tft.setCursor(10, 10);
    tft.print("INTERNET RADIO");
  }

  // Tunn avdelningslinje under header
  tft.drawFastHLine(10, 25, 300, COLOR_DIM_GRAY);

  // 2. Aktuell station (Stort och rent utan ramar)
  tft.setTextColor(COLOR_GRAY);
  tft.setTextSize(1);
  tft.setCursor(10, 42);
  tft.print("SPELAS NU:");

  tft.setTextColor(COLOR_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 62);
  tft.print(stations[currentStationIndex].name);

  // Bitrate till höger
  tft.setTextSize(1);
  tft.setTextColor(COLOR_ORANGE);
  tft.setCursor(240, 42);
  tft.print(stations[currentStationIndex].bitrate);

  // Tunn avdelningslinje i mitten
  tft.drawFastHLine(10, 95, 300, COLOR_DIM_GRAY);

  // 3. Botten: Volymstapel & WiFi
  tft.setTextColor(COLOR_GRAY);
  tft.setTextSize(1);
  tft.setCursor(10, 115);
  tft.print("Volym");

  // Volymstapel
  tft.drawRect(55, 114, 130, 12, COLOR_DIM_GRAY);
  int volPercent = (int)(currentGain * 100);
  int barWidth = map(volPercent, 0, 100, 0, 126);
  tft.fillRect(57, 116, barWidth, 8, COLOR_GREEN);
  
  tft.setCursor(195, 115);
  tft.setTextColor(COLOR_WHITE);
  tft.print(volPercent);
  tft.print("%");

  // RSSI till höger
  tft.setTextColor(COLOR_CYAN);
  tft.setCursor(235, 115);
  tft.print("RSSI:");
  tft.print(lastKnownRssi);

  // 4. Statusrad längst ner
  tft.setTextColor(COLOR_DIM_GRAY);
  tft.setCursor(10, 150);
  tft.print(WiFi.status() == WL_CONNECTED ? "Status: Ansluten" : "Status: Ansluter...");
}

// Flimmerfri uppdatering ENDAST för stationsbyte
void updateStationDisplay() {
  // Rensa endast ytan där stationsnamnet och bitraten finns
  tft.fillRect(10, 40, 300, 45, COLOR_BG);

  tft.setTextColor(COLOR_GRAY);
  tft.setTextSize(1);
  tft.setCursor(10, 42);
  tft.print("SPELAS NU:");

  tft.setTextColor(COLOR_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 62);
  tft.print(stations[currentStationIndex].name);

  tft.setTextSize(1);
  tft.setTextColor(COLOR_ORANGE);
  tft.setCursor(240, 42);
  tft.print(stations[currentStationIndex].bitrate);
}

// Flimmerfri uppdatering ENDAST för volymändring
void updateVolumeDisplay() {
  int volPercent = (int)(currentGain * 100);
  
  // Rensa stapelns insida och procent-texten
  tft.fillRect(57, 116, 126, 8, COLOR_BG); 
  int barWidth = map(volPercent, 0, 100, 0, 126);
  tft.fillRect(57, 116, barWidth, 8, COLOR_GREEN);

  tft.fillRect(195, 115, 35, 10, COLOR_BG);
  tft.setCursor(195, 115);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_WHITE);
  tft.print(volPercent);
  tft.print("%");
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
      updateDisplay(); // Ritar om hela skärmen vid lägesbyte för att visa rätt rubrik
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
      updateStationDisplay(); // Flimmerfritt byte av station!
    } 
    else if (currentMode == MODE_VOLUME) {
      currentGain += (diff > 0 ? 0.05 : -0.05);
      if (currentGain > 1.0) currentGain = 1.0;
      if (currentGain < 0.0) currentGain = 0.0;
      
      if (out) {
        out->SetGain(currentGain);
      }
      updateVolumeDisplay(); // Flimmerfri volymändring!
    }
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
