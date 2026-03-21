# תוכנית בדיקת OTA עם HackRF One

## ציוד זמין
| פריט | כמות | שימוש |
|------|-------|-------|
| HackRF One | 1 | TX/RX SDR |
| Attenuator 10dB | 1 | התאמת רמת אות |
| Attenuator 20dB | 1 | התאמת רמת אות |
| Attenuator 30dB | 1 | התאמת רמת אות |
| SMA male ↔ SMA male | 2 | חיבור בין רכיבים |
| Dummy Load 50Ω | 2 | סיום קו/בליעת אות |
| Logic Analyzer | 1 | דיבוג SPI/timing |

---

## שלב 1: בדיקת לולאה סגורה (Conducted Test) - 15 דקות

### מטרה
לוודא שה-Transceiver שלך מייצר ומקבל LoRa תקין לפני שעוברים לאוויר.

### חיבור פיזי
```
┌─────────────────┐     ┌──────────────┐     ┌─────────────────┐
│  TX Generator   │────▶│  30dB + 20dB │────▶│   HackRF One    │
│  (your code)    │     │  Attenuators │     │   (RX mode)     │
└─────────────────┘     └──────────────┘     └─────────────────┘
        │                                            │
        └────────── SMA cables ──────────────────────┘
```

**הערה:** אם אתה משתמש ב-HackRF גם ל-TX וגם ל-RX, נצטרך שתי הקלטות נפרדות.

### סקריפט הקלטה בסיסי

```bash
#!/bin/bash
# hackrf_capture_lora.sh

FREQ=868100000      # 868.1 MHz (EU LoRa)
SAMPLE_RATE=2000000 # 2 MSPS
GAIN_LNA=32         # LNA gain dB
GAIN_VGA=20         # VGA gain dB
DURATION=5          # seconds

hackrf_transfer -r capture_ota.raw \
    -f $FREQ \
    -s $SAMPLE_RATE \
    -l $GAIN_LNA \
    -g $GAIN_VGA \
    -n $((SAMPLE_RATE * DURATION))

# המר ל-CF32 (complex float)
python3 -c "
import numpy as np
raw = np.fromfile('capture_ota.raw', dtype=np.int8)
iq = raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32)
iq /= 128.0  # normalize
iq.astype(np.complex64).tofile('capture_ota.cf32')
print(f'Converted {len(iq)} samples to capture_ota.cf32')
"
```

---

## שלב 2: בדיקת תיאמות מהירה מול GNU Radio - 10 דקות

### מטרה
לוודא שה-decoder שלך וה-GNU Radio מפענחים אותו אות זהה.

### הפעלה

```bash
# 1. הקלט אות LoRa (מה-Transceiver שלך או ממקור אחר)
./hackrf_capture_lora.sh

# 2. צור metadata JSON
cat > capture_metadata.json << EOF
{
    "sf": 7,
    "bw": 125000,
    "cr": 1,
    "crc_enabled": true,
    "implicit_header": false,
    "sample_rate": 2000000
}
EOF

# 3. השווה מול GNU Radio
python3 tools/compare_hackrf_capture.py \
    --iq capture_ota.cf32 \
    --metadata capture_metadata.json \
    --output-dir build/hackrf_test/
```

---

## שלב 3: סקריפט בדיקה אוטומטי

### קובץ: `tools/hackrf_quick_test.py`
סקריפט שמבצע:
1. המרת IQ מ-HackRF format ל-CF32
2. זיהוי פרמטרי LoRa (SF, BW)
3. פענוח עם הקוד שלך
4. פענוח עם GNU Radio
5. השוואת תוצאות

---

## שלב 4: מטריצת בדיקות מורחבת

| בדיקה | SF | BW | הנחתה | צפי |
|-------|----|----|-------|-----|
| Quick sanity | 7 | 125k | 50dB | שניהם מפענחים |
| Low signal | 7 | 125k | 60dB | שניהם מפענחים |
| Near threshold | 12 | 125k | 60dB | SF12 שורד יותר רעש |
| Wide BW | 7 | 500k | 50dB | בדיקת BW גבוה |

---

## בדיקת בריאות מהירה (Smoke Test)

רוץ את זה כדי לוודא שהכל עובד:

```bash
# 1. בדוק ש-HackRF מחובר
hackrf_info

# 2. הקלטה קצרה (1 שנייה)
hackrf_transfer -r /tmp/test.raw -f 868100000 -s 2000000 -n 2000000

# 3. בדוק שיש נתונים
ls -la /tmp/test.raw

# 4. נתח את הספקטרום
python3 << 'EOF'
import numpy as np
raw = np.fromfile('/tmp/test.raw', dtype=np.int8)
iq = raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32)
power = 10 * np.log10(np.mean(np.abs(iq)**2) + 1e-10)
print(f"Mean power: {power:.1f} dB")
print(f"Samples: {len(iq)}")
EOF
```

---

## הערות חשובות

### רמות הנחתה
- **50dB (30+20)**: בדיקה רגילה, SNR גבוה
- **60dB (30+20+10)**: סימולציה של אות חלש
- **עם Dummy Load**: מונע קרינה לאוויר (בבדיקות מעבדה)

### תדרים חוקיים
- EU: 868.0 - 868.6 MHz (duty cycle 1%)
- **בבדיקות מעבדה עם Dummy Load** - אין בעיה רגולטורית

### Logic Analyzer
שמור לשלב הבא - דיבוג תזמון SPI בין MCU ל-Radio chip.

---

## הצעד הבא

לאחר שהבדיקה הבסיסית עוברת, נעבור ל:
1. בדיקות BER מול GNU Radio
2. בדיקות impairment (CFO, SFO, STO)
3. בדיקות OTA אמיתיות (עם אנטנות)
