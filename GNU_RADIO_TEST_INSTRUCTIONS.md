# בדיקת וקטור GNU Radio עם GRC

## שלבים לבדיקת הוקטור:

### 1. פתח GNU Radio Companion
```bash
gnuradio-companion
```

### 2. טען את הפלואגרף
- פתח את הקובץ `test_lora_vector.grc`
- או צור פלואגרף חדש עם הבלוקים הבאים:

### 3. בלוקים נדרשים (לפי הסדר):
1. **File Source** (blocks_file_source)
   - File: `vectors/gnu_radio_real_vector.bin`
   - Type: Complex
   - Repeat: False

2. **LoRa Frame Sync** (lora_sdr_frame_sync)
   - Center Freq: 0
   - Bandwidth: 125000
   - SF: 7
   - Impl Head: False (explicit header)
   - Sync Word: [0x1, 0x2] (לנסות גם [0x3, 0x4])
   - OS Factor: 1
   - Preamble Len: 8

3. **LoRa FFT Demod** (lora_sdr_fft_demod)
   - Soft Decoding: False
   - Max Log Approx: False

4. **LoRa Gray Mapping** (lora_sdr_gray_mapping)
   - Soft Decoding: False

5. **LoRa Deinterleaver** (lora_sdr_deinterleaver)
   - Soft Decoding: False

6. **LoRa Hamming Dec** (lora_sdr_hamming_dec)
   - Soft Decoding: False

7. **LoRa Dewhitening** (lora_sdr_dewhitening)
   - (ללא פרמטרים)

8. **LoRa Header Decoder** (lora_sdr_header_decoder)
   - Impl Head: False
   - CR: 1 (4/5)
   - Pay Len: 255
   - Has CRC: True
   - LDRO: 0
   - Print Header: True

9. **LoRa CRC Verif** (lora_sdr_crc_verif)
   - Print RX Msg: 1
   - Output CRC Check: True

10. **Message Debug** (blocks_message_debug)
    - En UVec: True

### 4. חיבורים:
- File Source → Frame Sync
- Frame Sync → FFT Demod
- FFT Demod → Gray Mapping
- Gray Mapping → Deinterleaver
- Deinterleaver → Hamming Dec
- Hamming Dec → Dewhitening
- Dewhitening → Header Decoder
- Header Decoder → CRC Verif
- CRC Verif [msg] → Message Debug [print]

### 5. הרצה:
1. לחץ על Generate (F5)
2. לחץ על Execute (F6)
3. בדוק את ה-console output וה-Message Debug

### 6. אם לא עובד - נסה:
- Sync Word שונה: [0x3, 0x4], [0x2, 0x1], [0x4, 0x3]
- CR שונה: 0, 2, 3, 4
- SF שונה: 8, 9, 10
- Preamble Len שונה: 6, 10, 12

### 7. סימנים לפענוח מוצלח:
- הודעות בMessage Debug
- Header info מודפס בconsole
- ללא שגיאות בconsole

### 8. אם עדיין לא עובד:
- ייתכן שהוקטור פגום או לא מכיל LoRa frames תקינים
- כדאי לנסות ליצור וקטור חדש עם GNU Radio
