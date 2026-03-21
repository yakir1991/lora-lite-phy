/*
 * RFM95W LoRa RX Test
 * 
 * Receives LoRa packets and prints them to Serial.
 * Use this to verify HackRF TX is working.
 * 
 * Wiring: Same as TX test
 * 
 * Install RadioLib library from Arduino Library Manager!
 */

#include <RadioLib.h>

// Arduino UNO pins
// Arduino UNO pins (uncomment if using Arduino)
// #define PIN_NSS   10
// #define PIN_DIO0  2
// #define PIN_RST   9
// #define PIN_DIO1  -1

// ESP32 pins
#define PIN_NSS   5   // D5
#define PIN_DIO0  26  // D26
#define PIN_RST   14  // D14
#define PIN_DIO1  -1

RFM95 radio = new Module(PIN_NSS, PIN_DIO0, PIN_RST, PIN_DIO1);

// LoRa parameters - MUST MATCH TX SIDE
#define LORA_FREQUENCY    868.1
#define LORA_BANDWIDTH    125.0
#define LORA_SF           7
#define LORA_CR           5
#define LORA_SYNC_WORD    0x12
#define LORA_PREAMBLE     8

uint32_t rxCount = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial);
  
  Serial.println(F("====================================="));
  Serial.println(F("  RFM95W LoRa RX Test"));
  Serial.println(F("====================================="));
  
  Serial.print(F("[RFM95] Initializing... "));
  int state = radio.begin(
    LORA_FREQUENCY,
    LORA_BANDWIDTH,
    LORA_SF,
    LORA_CR,
    LORA_SYNC_WORD,
    10,  // TX power (not used in RX)
    LORA_PREAMBLE
  );
  
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println(F("OK!"));
  } else {
    Serial.print(F("FAILED! Error: "));
    Serial.println(state);
    while (true);
  }
  
  Serial.println(F("\nConfiguration:"));
  Serial.print(F("  Frequency: ")); Serial.print(LORA_FREQUENCY); Serial.println(F(" MHz"));
  Serial.print(F("  Bandwidth: ")); Serial.print(LORA_BANDWIDTH); Serial.println(F(" kHz"));
  Serial.print(F("  SF: ")); Serial.println(LORA_SF);
  Serial.print(F("  CR: 4/")); Serial.println(LORA_CR);
  Serial.println(F("\nWaiting for packets...\n"));
}

void loop() {
  // Try to receive a packet
  String received;
  int state = radio.receive(received);
  
  if (state == RADIOLIB_ERR_NONE) {
    rxCount++;
    
    Serial.println(F("┌────────────────────────────────────┐"));
    Serial.print(F("│ [RX #")); 
    Serial.print(rxCount);
    Serial.println(F("] Packet received!"));
    Serial.println(F("├────────────────────────────────────┤"));
    
    // Print packet info
    Serial.print(F("│ Data: \""));
    Serial.print(received);
    Serial.println(F("\""));
    
    Serial.print(F("│ Length: "));
    Serial.print(received.length());
    Serial.println(F(" bytes"));
    
    Serial.print(F("│ RSSI: "));
    Serial.print(radio.getRSSI());
    Serial.println(F(" dBm"));
    
    Serial.print(F("│ SNR: "));
    Serial.print(radio.getSNR());
    Serial.println(F(" dB"));
    
    // Print hex dump
    Serial.print(F("│ Hex: "));
    for (int i = 0; i < min((int)received.length(), 16); i++) {
      if (received[i] < 0x10) Serial.print('0');
      Serial.print((uint8_t)received[i], HEX);
      Serial.print(' ');
    }
    if (received.length() > 16) Serial.print(F("..."));
    Serial.println();
    
    Serial.println(F("└────────────────────────────────────┘\n"));
    
  } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
    Serial.println(F("[RX] CRC error!"));
  }
  // RADIOLIB_ERR_RX_TIMEOUT is normal when no packet
}
