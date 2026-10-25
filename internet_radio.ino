#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <AudioFileSourceICYStream.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>

//#include "config.h"

// --- WI-FI-UPPGIFTER ---
//const char* ssid     = "DIN_SSID";       // Byt ut mot ditt nätverk
//const char* password = "DITT_LOSENORD";   // Byt ut mot ditt lösenord

// --- STIFTDEFINTIONER ---
#define SDA_PIN 8
#define SCL_PIN 7

#define ENCODER_A  20  // U0RXD (Ingen Serial får användas här!)
#define ENCODER_B  21  // U0txd (Ingen Serial får användas här!)
#define ENCODER_SW  4   // Tryckknapp

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

volatile long encoderPosition = 0;
volatile int lastEncoded = 0;
long lastProcessedPosition = 0;

AudioFileSourceICYStream *file = NULL;
AudioFileSourceBuffer *buff = NULL;
AudioGeneratorMP3 *mp3 = NULL;
AudioOutputI2S *out = NULL;
const int BUFFER_SIZE = 1536;

void IRAM_ATTR readEncoder() {
  int A = digitalRead(ENCODER_A);
  int B = digitalRead(ENCODER_B);

  int encoded = (A << 1) | B;
  int sum = (lastEncoded << 2) | encoded;

  if (sum == 0b1101 || sum == 0b0100 || sum == 0b0010 || sum == 0b1011) {
    encoderPosition++;
  }
  if (sum == 0b1110 || sum == 0b0111 || sum == 0b0001 || sum == 0b1000) {
    encoderPosition--;
  }
  lastEncoded = encoded;
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  // Visa på skärmen att vi ansluter till Wi-Fi
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 20);
  display.println("Ansluter till Wi-Fi...");
  display.display();

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
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
  }
}

void updateDisplay() {
  display.clearDisplay();
  
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.println("ESP32-C3 Internet Radio");
  display.drawLine(0, 10, 128, 10, SH110X_WHITE);

  display.setCursor(0, 18);
  display.print("Kanal ");
  display.print(currentStationIndex + 1);
  display.print("/");
  display.println(totalStations);

  display.setCursor(0, 32);
  display.println(stations[currentStationIndex].name);

  display.setCursor(0, 52);
  if (WiFi.status() == WL_CONNECTED) {
    display.print("Status: Spelar");
  } else {
    display.print("Status: Ansluter...");
  }

  display.display();
}

void setup() {
  // OBS: Ingen Serial.begin() här eftersom pinnar 20 och 21 används av enkodern!

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
  out->SetGain(0.4);

  startAudio();
  updateDisplay();
}

void loop() {
  // 1. Kolla om enkodern har vridits
  noInterrupts();
  long currentPos = encoderPosition;
  interrupts();

  if (currentPos != lastProcessedPosition) {
    int diff = currentPos - lastProcessedPosition;
    lastProcessedPosition = currentPos;

    currentStationIndex = (currentStationIndex + (diff > 0 ? 1 : -1)) % totalStations;
    if (currentStationIndex < 0) {
      currentStationIndex = totalStations - 1;
    }

    startAudio();
    updateDisplay();
  }

  // 2. Wi-Fi kontroll
  if (WiFi.status() != WL_CONNECTED) {
    stopAudio();
    connectWiFi();
    startAudio();
    updateDisplay();
    return;
  }

  // 3. Ljudloop
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

  // Låt systemet andas så Watchdog inte utlöses
  vTaskDelay(1); 
}
