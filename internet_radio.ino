#include <WiFi.h>
#include <AudioFileSourceICYStream.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>

#include "config.h"
//const char* ssid     = "DIN_SSID";     // Byt ut mot ditt nätverk
//const char* password = "DITT_LOSENORD"; // Byt ut mot ditt lösenord

// Struktur för att hålla ordning på radiokanaler
struct RadioStation {
  const char* name;
  const char* url;
};

// En utökad lista med Sveriges Radios kanaler (MP3-strömmar)
RadioStation stations[] = {
  {"1. Sveriges Radio P1",     "http://live1.sr.se/p1-mp3-96"},
  {"2. Sveriges Radio P2",     "http://live1.sr.se/p2-mp3-96"},
  {"3. Sveriges Radio P3",     "http://live1.sr.se/p3-mp3-96"},
  {"4. P4 Stockholm",          "http://live1.sr.se/p4sth-mp3-96"},
  {"5. P4 Uppland",            "http://live1.sr.se/p4uppl-mp3-96"},
  {"6. P4 Gotland",            "http://live1.sr.se/p4gotl-mp3-96"},
  {"7. SR P4 Plus",            "http://live1.sr.se/p4plus-mp3-96"},
  {"8. SR Din Gata",            "http://live1.sr.se/dingata-mp3-96"}
};

const int totalStations = sizeof(stations) / sizeof(stations[0]);
int currentStationIndex = 0;

AudioFileSourceICYStream *file = NULL;
AudioFileSourceBuffer *buff = NULL;
AudioGeneratorMP3 *mp3 = NULL;
AudioOutputI2S *out = NULL;

const int BUFFER_SIZE = 16384;

void connectWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("Ansluter till Wi-Fi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempts++;
    if (attempts > 40) {
      Serial.println("\nKunde inte ansluta till Wi-Fi. Startar om...");
      ESP.restart();
    }
  }
  Serial.println("\nAnsluten!");
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
  Serial.println("\n----------------------------------------");
  Serial.print("Spelar nu: ");
  Serial.println(stations[currentStationIndex].name);
  Serial.println("----------------------------------------");

  file = new AudioFileSourceICYStream(stations[currentStationIndex].url);
  buff = new AudioFileSourceBuffer(file, BUFFER_SIZE);
  mp3 = new AudioGeneratorMP3();
  
  if (!mp3->begin(buff, out)) {
    Serial.println("Kunde inte starta MP3-avkodaren!");
  }
}

void printStationList() {
  Serial.println("\n=== TILLGÄNGLIGA KANALER ===");
  for (int i = 0; i < totalStations; i++) {
    Serial.println(stations[i].name);
  }
  Serial.println("Skriv kanalnummer (1-" + String(totalStations) + ") och tryck Enter för att byta.\n");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  connectWiFi();

  // Konfigurera I2S-utgång för MAX98357A på ESP32-C3
  // Pinnar: BCLK = 6, WS/LRC = 7, DOUT = 8
  out = new AudioOutputI2S();
  out->SetPinout(6, 7, 8);
  out->SetGain(0.4);

  printStationList();
  startAudio();
}

void loop() {
  // 1. Hantera kanalbyte via Seriell-monitorn (stödjer nu flera siffror + Enter)
  if (Serial.available() > 0) {
    int channelInput = Serial.parseInt(); // Läser heltal från inmatningen
    
    // Rensa bort eventuella skräptecken (som radbrytningar) i bufferten
    while (Serial.available() > 0) {
      Serial.read();
    }

    if (channelInput > 0) {
      int selectedIndex = channelInput - 1; // Konvertera från 1-baserat till 0-baserat index
      if (selectedIndex < totalStations) {
        currentStationIndex = selectedIndex;
        stopAudio();
        delay(300);
        startAudio();
      } else {
        Serial.println("Ogiltigt kanalnummer! Välj mellan 1 och " + String(totalStations));
      }
    }
  }

  // 2. Kontrollera Wi-Fi-status
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi tappades! Återansluter...");
    stopAudio();
    connectWiFi();
    startAudio();
    return;
  }

  // 3. Ljudspelarens loop
  if (mp3 && mp3->isRunning()) {
    if (!mp3->loop()) {
      Serial.println("Strömmen avbröts. Försöker starta om...");
      stopAudio();
      delay(2000);
      startAudio();
    }
  } else {
    Serial.println("Spelaren är inaktiv. Startar om om 3 sekunder...");
    stopAudio();
    delay(3000);
    startAudio();
  }
}
