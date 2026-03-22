/*
 * RFM95W LoRa Parameter Sweep TX
 *
 * Serial-controlled transmitter for automated OTA testing.
 * Accepts commands to change SF/BW/CR/power and transmit packets
 * without reflashing.
 *
 * Serial protocol (115200 baud):
 *   Host → ESP32:
 *     "SET SF=7 BW=125 CR=5 PWR=10\n"  → reconfigure radio
 *     "TX 5\n"                          → transmit 5 packets
 *     "TX 1 Hello World\n"              → transmit 1 packet with custom payload
 *     "STATUS\n"                        → print current config
 *     "RESET\n"                         → reset to defaults
 *
 *   ESP32 → Host:
 *     "OK SET SF=7 BW=125 CR=5 PWR=10\n"
 *     "OK TX 5\n"
 *     "SENT 1/5 \"LoRa Test #42\"\n"
 *     "DONE TX 5\n"
 *     "ERR <message>\n"
 *
 * Wiring: same as rfm95_tx_test (ESP32 pins by default).
 * Install RadioLib from Arduino Library Manager.
 */

#include <RadioLib.h>

// ----- Pin definitions (ESP32) -----
#define PIN_NSS   5
#define PIN_DIO0  26
#define PIN_RST   14
#define PIN_DIO1  -1

RFM95 radio = new Module(PIN_NSS, PIN_DIO0, PIN_RST, PIN_DIO1);

// ----- Current LoRa config -----
struct LoRaConfig {
  float    freq       = 868.1;   // MHz
  float    bw         = 125.0;   // kHz
  uint8_t  sf         = 7;       // 5-12
  uint8_t  cr         = 5;       // 5-8  (meaning 4/5 .. 4/8)
  uint8_t  syncWord   = 0x12;
  int8_t   power      = 10;      // dBm
  uint16_t preamble   = 8;
  uint32_t intervalMs = 1000;    // ms between packets in a burst
};

LoRaConfig cfg;
uint32_t globalPacketCount = 0;
bool radioReady = false;

// ----- Apply config to radio -----
bool applyConfig() {
  int state = radio.begin(
    cfg.freq,
    cfg.bw,
    cfg.sf,
    cfg.cr,
    cfg.syncWord,
    cfg.power,
    cfg.preamble
  );
  radioReady = (state == RADIOLIB_ERR_NONE);
  if (radioReady && cfg.sf <= 6) {
    // SX1276 requires implicit header mode for SF6
    radio.implicitHeader(255);
  } else if (radioReady) {
    radio.explicitHeader();
  }
  return radioReady;
}

// ----- Print current config -----
void printConfig() {
  Serial.print(F("CONFIG SF="));   Serial.print(cfg.sf);
  Serial.print(F(" BW="));        Serial.print((int)cfg.bw);
  Serial.print(F(" CR=4/"));      Serial.print(cfg.cr);
  Serial.print(F(" PWR="));       Serial.print(cfg.power);
  Serial.print(F(" FREQ="));      Serial.print(cfg.freq, 1);
  Serial.print(F(" SYNC=0x"));    Serial.print(cfg.syncWord, HEX);
  Serial.print(F(" PRE="));       Serial.print(cfg.preamble);
  Serial.print(F(" INT="));       Serial.print(cfg.intervalMs);
  Serial.print(F(" READY="));     Serial.println(radioReady ? F("YES") : F("NO"));
}

// ----- Parse SET command -----
// Format: SET SF=7 BW=125 CR=5 PWR=10 FREQ=868.1 SYNC=18 PRE=8 INT=1000
bool parseSetCommand(const String &line) {
  bool changed = false;
  int idx;

  idx = line.indexOf("SF=");
  if (idx >= 0) { cfg.sf = (uint8_t)line.substring(idx + 3).toInt(); changed = true; }

  idx = line.indexOf("BW=");
  if (idx >= 0) { cfg.bw = line.substring(idx + 3).toFloat(); changed = true; }

  idx = line.indexOf("CR=");
  if (idx >= 0) { cfg.cr = (uint8_t)line.substring(idx + 3).toInt(); changed = true; }

  idx = line.indexOf("PWR=");
  if (idx >= 0) { cfg.power = (int8_t)line.substring(idx + 4).toInt(); changed = true; }

  idx = line.indexOf("FREQ=");
  if (idx >= 0) { cfg.freq = line.substring(idx + 5).toFloat(); changed = true; }

  idx = line.indexOf("SYNC=");
  if (idx >= 0) { cfg.syncWord = (uint8_t)strtol(line.substring(idx + 5).c_str(), NULL, 0); changed = true; }

  idx = line.indexOf("PRE=");
  if (idx >= 0) { cfg.preamble = (uint16_t)line.substring(idx + 4).toInt(); changed = true; }

  idx = line.indexOf("INT=");
  if (idx >= 0) { cfg.intervalMs = (uint32_t)line.substring(idx + 4).toInt(); changed = true; }

  return changed;
}

// ----- Handle TX command -----
// Format: TX <count> [custom payload]
void handleTxCommand(const String &line) {
  if (!radioReady) {
    Serial.println(F("ERR radio not ready"));
    return;
  }

  // Parse count
  int spaceIdx = line.indexOf(' ', 3);
  int count = 1;
  String customPayload = "";

  if (line.length() > 3) {
    count = line.substring(3).toInt();
    if (count <= 0) count = 1;
    if (count > 100) count = 100;
  }

  // Check for custom payload after count
  if (spaceIdx > 3) {
    int secondSpace = line.indexOf(' ', spaceIdx + 1);
    if (secondSpace > spaceIdx) {
      customPayload = line.substring(secondSpace + 1);
    }
  }

  Serial.print(F("OK TX "));
  Serial.println(count);

  for (int i = 0; i < count; i++) {
    char message[64];
    if (customPayload.length() > 0) {
      snprintf(message, sizeof(message), "%s", customPayload.c_str());
    } else {
      snprintf(message, sizeof(message), "LoRa Test #%lu", globalPacketCount);
    }

    unsigned long t0 = millis();
    int state = radio.transmit(message);
    unsigned long dt = millis() - t0;

    Serial.print(F("SENT "));
    Serial.print(i + 1);
    Serial.print('/');
    Serial.print(count);
    Serial.print(F(" \""));
    Serial.print(message);
    Serial.print(F("\" "));
    Serial.print(dt);
    Serial.print(F("ms "));
    Serial.println(state == RADIOLIB_ERR_NONE ? F("OK") : F("FAIL"));

    globalPacketCount++;

    if (i < count - 1) {
      delay(cfg.intervalMs);
    }
  }

  Serial.print(F("DONE TX "));
  Serial.println(count);
}

// ----- Process one command line -----
void processCommand(const String &line) {
  String trimmed = line;
  trimmed.trim();
  if (trimmed.length() == 0) return;

  if (trimmed.startsWith("SET ")) {
    if (parseSetCommand(trimmed)) {
      if (applyConfig()) {
        Serial.print(F("OK SET "));
        Serial.print(F("SF="));  Serial.print(cfg.sf);
        Serial.print(F(" BW=")); Serial.print((int)cfg.bw);
        Serial.print(F(" CR=")); Serial.print(cfg.cr);
        Serial.print(F(" PWR=")); Serial.println(cfg.power);
      } else {
        Serial.println(F("ERR radio.begin() failed after SET"));
      }
    } else {
      Serial.println(F("ERR no valid params in SET"));
    }
  }
  else if (trimmed.startsWith("TX")) {
    handleTxCommand(trimmed);
  }
  else if (trimmed == "STATUS") {
    printConfig();
  }
  else if (trimmed == "RESET") {
    cfg = LoRaConfig();
    if (applyConfig()) {
      Serial.println(F("OK RESET"));
    } else {
      Serial.println(F("ERR reset failed"));
    }
    printConfig();
  }
  else {
    Serial.print(F("ERR unknown command: "));
    Serial.println(trimmed);
  }
}

// ----- Setup -----
void setup() {
  Serial.begin(115200);
  while (!Serial);

  Serial.println(F("====================================="));
  Serial.println(F("  RFM95W LoRa Param Sweep TX"));
  Serial.println(F("====================================="));
  Serial.println(F("Commands: SET, TX, STATUS, RESET"));
  Serial.println(F("Example:  SET SF=9 BW=125 CR=5"));
  Serial.println(F("          TX 3"));
  Serial.println();

  if (applyConfig()) {
    Serial.println(F("READY"));
  } else {
    Serial.println(F("ERR init failed"));
  }
  printConfig();
}

// ----- Main loop: read Serial commands -----
String inputBuffer = "";

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (inputBuffer.length() > 0) {
        processCommand(inputBuffer);
        inputBuffer = "";
      }
    } else {
      inputBuffer += c;
      if (inputBuffer.length() > 128) {
        Serial.println(F("ERR line too long"));
        inputBuffer = "";
      }
    }
  }
}
