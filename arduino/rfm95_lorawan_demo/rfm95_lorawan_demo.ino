/*
 * RFM95W LoRaWAN Demo Frames
 *
 * Sends raw LoRaWAN-formatted frames (no stack, just raw bytes) for
 * security auditor testing.  Cycles through scenarios:
 *   1. Unconfirmed Data Up with incrementing FCnt  (normal)
 *   2. FCnt reset back to 0                        (security alert)
 *   3. JoinRequest with reused DevNonce             (security alert)
 *   4. Second device (different DevAddr)            (ABP detection)
 *
 * NOTE: MIC values are dummy (0xDEADBEEF).  This is intentional —
 * we are testing the auditor's parsing, not crypto validation.
 *
 * Wiring: same as rfm95_tx_test (ESP32 + RFM95W)
 */

#include <RadioLib.h>

// ESP32 pins
#define PIN_NSS   5
#define PIN_DIO0  26
#define PIN_RST   14
#define PIN_DIO1  -1

RFM95 radio = new Module(PIN_NSS, PIN_DIO0, PIN_RST, PIN_DIO1);

// LoRa params — match decoder settings
#define LORA_FREQUENCY    868.1
#define LORA_BANDWIDTH    125.0
#define LORA_SF           12       // SF12 for RFM95 default
#define LORA_CR           8        // CR 4/8
#define LORA_SYNC_WORD    0x34     // LoRaWAN public network
#define LORA_POWER        10
#define LORA_PREAMBLE     8

// ---- LoRaWAN MHDR types ----
#define MHDR_JOIN_REQUEST       0x00  // 000 | 000 | 00
#define MHDR_UNCONFIRMED_UP     0x40  // 010 | 000 | 00
#define MHDR_CONFIRMED_UP       0x80  // 100 | 000 | 00

// ---- Simulated device identities ----
static const uint8_t DEV_ADDR_1[] = {0x04, 0x03, 0x02, 0x01};  // 01020304 LE
static const uint8_t DEV_ADDR_2[] = {0x78, 0x56, 0x34, 0x12};  // 12345678 LE

static const uint8_t APP_EUI[]  = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};
static const uint8_t DEV_EUI[]  = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF,0x00,0x11};
static const uint8_t FAKE_MIC[] = {0xEF,0xBE,0xAD,0xDE};  // 0xDEADBEEF LE

uint16_t fcnt_dev1 = 0;
uint16_t fcnt_dev2 = 0;
uint8_t  scenario  = 0;   // cycle counter
uint16_t dev_nonce = 0x1234;  // fixed nonce to trigger reuse alert

// ---- Helper: build Unconfirmed Data Up ----
// Returns frame length
uint8_t buildDataUp(uint8_t *buf, const uint8_t *devAddr, uint16_t fcnt,
                    const uint8_t *payload, uint8_t payloadLen)
{
  uint8_t pos = 0;
  buf[pos++] = MHDR_UNCONFIRMED_UP;

  // DevAddr (4 bytes LE — already LE in our arrays)
  memcpy(buf + pos, devAddr, 4); pos += 4;

  // FCtrl: ADR=0, no FOpts
  buf[pos++] = 0x00;

  // FCnt (2 bytes LE)
  buf[pos++] = fcnt & 0xFF;
  buf[pos++] = (fcnt >> 8) & 0xFF;

  // FPort
  buf[pos++] = 0x01;

  // FRMPayload
  memcpy(buf + pos, payload, payloadLen); pos += payloadLen;

  // MIC (4 bytes, dummy)
  memcpy(buf + pos, FAKE_MIC, 4); pos += 4;

  return pos;
}

// ---- Helper: build JoinRequest ----
uint8_t buildJoinRequest(uint8_t *buf, uint16_t nonce)
{
  uint8_t pos = 0;
  buf[pos++] = MHDR_JOIN_REQUEST;

  // AppEUI (8 bytes LE)
  memcpy(buf + pos, APP_EUI, 8); pos += 8;

  // DevEUI (8 bytes LE)
  memcpy(buf + pos, DEV_EUI, 8); pos += 8;

  // DevNonce (2 bytes LE)
  buf[pos++] = nonce & 0xFF;
  buf[pos++] = (nonce >> 8) & 0xFF;

  // MIC (4 bytes, dummy)
  memcpy(buf + pos, FAKE_MIC, 4); pos += 4;

  return pos;
}

void printHex(const uint8_t *data, uint8_t len) {
  for (uint8_t i = 0; i < len; i++) {
    if (data[i] < 0x10) Serial.print('0');
    Serial.print(data[i], HEX);
  }
}

void sendFrame(const uint8_t *frame, uint8_t len, const char *desc) {
  Serial.print(F("[TX] "));
  Serial.print(desc);
  Serial.print(F("  hex="));
  printHex(frame, len);
  Serial.print(F(" ... "));

  int state = radio.transmit(frame, len);
  if (state == RADIOLIB_ERR_NONE) {
    Serial.println(F("OK"));
  } else {
    Serial.print(F("ERR ")); Serial.println(state);
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial);

  Serial.println(F("======================================="));
  Serial.println(F("  RFM95W LoRaWAN Demo Frame Generator"));
  Serial.println(F("======================================="));

  int state = radio.begin(LORA_FREQUENCY, LORA_BANDWIDTH, LORA_SF,
                          LORA_CR, LORA_SYNC_WORD, LORA_POWER, LORA_PREAMBLE);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print(F("Radio init FAILED: ")); Serial.println(state);
    while (true);
  }

  Serial.println(F("Radio OK — SF12 BW125 CR4/8 sync=0x34 (LoRaWAN)"));
  Serial.println(F("Scenarios: DataUp(dev1) x5, FCnt-reset, JoinReq x2 (nonce reuse), DataUp(dev2) x3"));
  Serial.println();
}

void loop() {
  uint8_t frame[64];
  uint8_t len;
  char desc[80];

  switch (scenario) {
    case 0 ... 4: {
      // 5 normal Unconfirmed Data Up from device 1
      const uint8_t payload[] = "Hello LoRaWAN";
      len = buildDataUp(frame, DEV_ADDR_1, fcnt_dev1, payload, sizeof(payload) - 1);
      snprintf(desc, sizeof(desc), "DataUp dev1 FCnt=%u", fcnt_dev1);
      sendFrame(frame, len, desc);
      fcnt_dev1++;
      break;
    }
    case 5: {
      // FCnt reset! (security alert)
      fcnt_dev1 = 0;
      const uint8_t payload[] = "FCnt Reset!";
      len = buildDataUp(frame, DEV_ADDR_1, fcnt_dev1, payload, sizeof(payload) - 1);
      snprintf(desc, sizeof(desc), "DataUp dev1 FCnt=%u ** RESET **", fcnt_dev1);
      sendFrame(frame, len, desc);
      fcnt_dev1++;
      break;
    }
    case 6: {
      // JoinRequest #1
      len = buildJoinRequest(frame, dev_nonce);
      snprintf(desc, sizeof(desc), "JoinReq nonce=0x%04X", dev_nonce);
      sendFrame(frame, len, desc);
      break;
    }
    case 7: {
      // JoinRequest #2 — same nonce! (nonce reuse alert)
      len = buildJoinRequest(frame, dev_nonce);
      snprintf(desc, sizeof(desc), "JoinReq nonce=0x%04X ** REUSE **", dev_nonce);
      sendFrame(frame, len, desc);
      break;
    }
    case 8 ... 10: {
      // 3 frames from device 2 (ABP detection — no Join)
      const uint8_t payload[] = "Device Two";
      len = buildDataUp(frame, DEV_ADDR_2, fcnt_dev2, payload, sizeof(payload) - 1);
      snprintf(desc, sizeof(desc), "DataUp dev2 FCnt=%u", fcnt_dev2);
      sendFrame(frame, len, desc);
      fcnt_dev2++;
      break;
    }
    default:
      // Cycle complete — restart
      Serial.println(F("\n--- Cycle complete, restarting ---\n"));
      scenario = 0;
      fcnt_dev1 = 0;
      fcnt_dev2 = 0;
      return;
  }

  scenario++;
  delay(5000);  // 5 seconds between frames
}
