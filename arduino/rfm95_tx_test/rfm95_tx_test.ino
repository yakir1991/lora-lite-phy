/*
 * RFM95W LoRa TX Test
 * 
 * Sends a LoRa packet every 5 seconds for testing with HackRF RX.
 * 
 * Wiring (Arduino UNO):
 *   RFM95W  ->  Arduino
 *   VCC     ->  3.3V (NOT 5V!)
 *   GND     ->  GND
 *   SCK     ->  D13
 *   MISO    ->  D12
 *   MOSI    ->  D11
 *   NSS     ->  D10
 *   RST     ->  D9
 *   DIO0    ->  D2
 * 
 * Wiring (ESP32):
 *   RFM95W  ->  ESP32
 *   VCC     ->  3.3V
 *   GND     ->  GND
 *   SCK     ->  GPIO18
 *   MISO    ->  GPIO19
 *   MOSI    ->  GPIO23
 *   NSS     ->  GPIO5
 *   RST     ->  GPIO14
 *   DIO0    ->  GPIO26
 * 
 * Install RadioLib library from Arduino Library Manager!
 */

#include <RadioLib.h>

// Arduino UNO pins (uncomment if using Arduino)
// #define PIN_NSS   10
// #define PIN_DIO0  2
// #define PIN_RST   9
// #define PIN_DIO1  -1

// ESP32 pins (D18=GPIO18, D19=GPIO19, etc.)
#define PIN_NSS   5   // D5
#define PIN_DIO0  26  // D26
#define PIN_RST   14  // D14
#define PIN_DIO1  -1  // Not used

// Create RFM95 instance
RFM95 radio = new Module(PIN_NSS, PIN_DIO0, PIN_RST, PIN_DIO1);

// LoRa parameters - MUST MATCH YOUR DECODER
#define LORA_FREQUENCY    868.1    // MHz
#define LORA_BANDWIDTH    125.0    // kHz
#define LORA_SF           7
#define LORA_CR           5        // Coding Rate
#define LORA_SYNC_WORD    0x12     // LoRa sync word (0x12 = private, 0x34 = LoRaWAN)
#define LORA_POWER        10       // TX power in dBm (2-20)
#define LORA_PREAMBLE     8        // Preamble length

uint32_t packetCount = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial);
  
  Serial.println(F("====================================="));
  Serial.println(F("  RFM95W LoRa TX Test"));
  Serial.println(F("====================================="));
  
  // Initialize RFM95
  Serial.print(F("[RFM95] Initializing... "));
  int state = radio.begin(
    LORA_FREQUENCY,
    LORA_BANDWIDTH,
    LORA_SF,
    LORA_CR,
    LORA_SYNC_WORD,
    LORA_POWER,
    LORA_PREAMBLE
  );
  
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println(F("OK!"));
  } else {
    Serial.print(F("FAILED! Error code: "));
    Serial.println(state);
    while (true);  // Halt
  }
  
  // Print configuration
  Serial.println(F("\nConfiguration:"));
  Serial.print(F("  Frequency: ")); Serial.print(LORA_FREQUENCY); Serial.println(F(" MHz"));
  Serial.print(F("  Bandwidth: ")); Serial.print(LORA_BANDWIDTH); Serial.println(F(" kHz"));
  Serial.print(F("  SF: ")); Serial.println(LORA_SF);
  Serial.print(F("  CR: 4/")); Serial.println(LORA_CR);
  Serial.print(F("  Power: ")); Serial.print(LORA_POWER); Serial.println(F(" dBm"));
  Serial.println(F("\nStarting TX loop...\n"));
}

void loop() {
  // Create test message
  char message[64];
  snprintf(message, sizeof(message), "LoRa Test #%lu", packetCount);
  
  Serial.print(F("[TX] Sending: "));
  Serial.print(message);
  Serial.print(F(" ... "));
  
  // Transmit
  unsigned long startTime = millis();
  int state = radio.transmit(message);
  unsigned long txTime = millis() - startTime;
  
  if (state == RADIOLIB_ERR_NONE) {
    Serial.print(F("OK! ("));
    Serial.print(txTime);
    Serial.println(F(" ms)"));
  } else {
    Serial.print(F("FAILED! Error: "));
    Serial.println(state);
  }
  
  packetCount++;
  
  // Wait before next transmission
  delay(5000);  // 5 seconds
}
