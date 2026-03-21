# Arduino LoRa Test Setup

## Hardware Required
- Arduino UNO/Nano or ESP32
- RFM95W 868MHz Module
- Antenna 868MHz
- Breadboard + Jumper wires

## Wiring

### Arduino UNO/Nano
| RFM95W | Arduino |
|--------|---------|
| VCC | **3.3V** ⚠️ |
| GND | GND |
| SCK | D13 |
| MISO | D12 |
| MOSI | D11 |
| NSS | D10 |
| RST | D9 |
| DIO0 | D2 |

### ESP32
| RFM95W | ESP32 (GPIO) | ESP32 (D label) |
|--------|--------------|-----------------|
| VCC | 3.3V | 3.3V |
| GND | GND | GND |
| SCK | GPIO18 | D18 |
| MISO | GPIO19 | D19 |
| MOSI | GPIO23 | D23 |
| NSS | GPIO5 | D5 |`
| RST | GPIO14 | D14 |
| DIO0 | GPIO26 | D26 |

> 💡 **GPIO18 = D18** - זה אותו פין, רק סימון שונה על הבורד

## Software Setup

1. Open Arduino IDE
2. Go to **Tools → Manage Libraries**
3. Search for **RadioLib**
4. Install **RadioLib by Jan Gromes**

## Test Procedure

### Test 1: RFM95W TX → HackRF RX
1. Upload `rfm95_tx_test.ino` to Arduino
2. Connect RFM95 antenna → attenuators → HackRF
3. Run: `./tools/hackrf_capture_lora.sh`
4. Check if decoder detects packets

### Test 2: HackRF TX → RFM95W RX
1. Upload `rfm95_rx_test.ino` to Arduino
2. Connect HackRF → attenuators → RFM95 antenna
3. Run: `python tools/hackrf_ota_loopback.py --iq <file> --mode tx_only`
4. Check Serial Monitor for received packets

### Test 3: Full Loopback
1. RFM95 #1 (TX) → attenuators → RFM95 #2 (RX)
2. Monitor both Serial outputs

## LoRa Parameters (Default)
- Frequency: 868.1 MHz
- Bandwidth: 125 kHz
- SF: 7
- CR: 4/5
- Sync Word: 0x12
- Preamble: 8 symbols

## Troubleshooting

### "FAILED! Error code: -2"
- Check wiring
- Verify 3.3V power (not 5V!)
- Check SPI connections

### No packets received
- Verify both sides use same parameters
- Check antenna connection
- Reduce attenuation

### CRC errors
- Check SF/BW/CR match on both sides
- Reduce distance/add attenuation
