#include <WiFi.h>
#include <AudioFileSourceICYStream.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>

const char* ssid     = "";
const char* password = "";

// En fungerande MP3-radiostöm
const char* URL = "http://live1.sr.se/p1-mp3-96";

AudioFileSourceICYStream *file = NULL;
AudioFileSourceBuffer *buff = NULL;
AudioGeneratorMP3 *mp3 = NULL;
AudioOutputI2S *out = NULL;

void setup() {
  Serial.begin(115200);

  WiFi.begin(ssid, password);
  Serial.print("Ansluter till Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nAnsluten!");

  // Konfigurera I2S-utgång för MAX98357A på ESP32-C3
  // Pinnar: BCLK = 6, WS/LRC = 7, DOUT = 8
  out = new AudioOutputI2S();
  out->SetPinout(6, 7, 8);
  out->SetGain(0.4); // Volym (0.0 till 1.0)

  file = new AudioFileSourceICYStream(URL);
  buff = new AudioFileSourceBuffer(file, 2048);
  mp3 = new AudioGeneratorMP3();
  mp3->begin(buff, out);
}

void loop() {
  if (mp3->isRunning()) {
    if (!mp3->loop()) {
      mp3->stop();
    }
  } else {
    delay(1000);
  }
}
