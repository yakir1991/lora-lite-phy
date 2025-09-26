#!/usr/bin/env python3
"""
חילוץ סמלים משופר עם שילוב מהטעון הקיים
"""
import os
import subprocess
import numpy as np
import struct

def run_frame_sync():
    """הרצת FrameSync והחזרת התוצאות"""
    print("🎯 מריץ FrameSync...")
    
    result = subprocess.run([
        './build_standalone/test_with_correct_sync', 
        'temp/hello_world.cf32',
        '7'  # SF parameter
    ], capture_output=True, text=True)
    
    if result.returncode != 0:
        print(f"❌ שגיאה בהרצת FrameSync: {result.stderr}")
        return None
        
    lines = result.stdout.strip().split('\n')
    sync_info = {}
    
    for line in lines:
        if 'FRAME DETECTED' in line and 'cfo_int' in line:
            # פרסור השורה: *** FRAME DETECTED! *** cfo_int=9, cfo_frac=0.00164447, sf=7
            parts = line.split()
            for part in parts:
                if 'cfo_int=' in part:
                    sync_info['cfo_int'] = int(part.split('=')[1].rstrip(','))
                elif 'cfo_frac=' in part:
                    sync_info['cfo_frac'] = float(part.split('=')[1].rstrip(','))
                    
        elif 'Iteration 8:' in line and 'frame_detected=1' in line:
            # Iteration 8: consumed=676, frame_detected=1, symbol_ready=0, snr_est=-16.9468
            parts = line.split()
            for part in parts:
                if 'consumed=' in part:
                    sync_info['consumed_at_detection'] = int(part.split('=')[1].rstrip(','))
                elif 'snr_est=' in part:
                    sync_info['snr'] = float(part.split('=')[1])
            sync_info['iteration'] = 8
            
        elif 'Total consumed:' in line:
            consumed = int(line.split()[-2])
            sync_info['total_consumed'] = consumed
    
    return sync_info

def calculate_precise_positions(sync_info):
    """חישוב מיקומים מדויקים על בסיס תוצאות הסנכרון"""
    
    if not sync_info:
        return []
    
    print("📊 מידע סנכרון:")
    for key, value in sync_info.items():
        print(f"   {key}: {value}")
    
    # חישוב מיקום מדויק
    iteration = sync_info.get('iteration', 8)
    
    # כל איטרציה: 512 דגימות (חוץ מ-3 ו-8)
    samples_consumed = 0
    for i in range(iteration):
        if i == 3:
            samples_consumed += 292
        elif i == 8:
            samples_consumed += 676  
        else:
            samples_consumed += 512
    
    # בסיס: מיקום זיהוי הפריים
    detection_pos = samples_consumed
    
    # LoRa frame structure:
    # 1. Preamble: 8 symbols (8 * 128 symbols * 4 sps = 4096 samples)
    # 2. Sync word: 2.25 symbols (2.25 * 128 * 4 = 1152 samples) 
    # 3. Header starts after
    
    preamble_samples = 8 * 128 * 4  # 4096
    sync_samples = int(2.25 * 128 * 4)  # 1152
    
    # המיקום שבו צריך להתחיל לחלץ סמלים
    header_start = detection_pos + preamble_samples + sync_samples
    
    print(f"📍 מיקום זיהוי: {detection_pos}")
    print(f"📦 תחילת header משוערת: {header_start}")
    
    # רשימת מיקומים לבדיקה
    positions = [
        header_start,
        header_start - 512,
        header_start + 512,
        header_start - 1024,
        header_start + 1024,
        10976,  # המיקום הטוב שמצאנו
        4552,   # מיקום נוסף
        detection_pos + 1000,  # מיקום יחסי
        detection_pos + 2000,
        detection_pos + 3000,
    ]
    
    return [pos for pos in positions if pos > 0]

def extract_symbols_advanced(samples, pos, cfo_int=9, cfo_frac=0.00164447):
    """חילוץ סמלים משופר עם תיקון CFO מדויק"""
    
    fs = 500000  # sample rate
    N = 128      # FFT size
    sps = 512    # samples per symbol (4 * N)
    
    # תיקון CFO
    cfo_hz = cfo_int * (fs / (2**7)) + cfo_frac * fs  # CFO ב-Hz
    t = np.arange(len(samples)) / fs
    cfo_correction = np.exp(-1j * 2 * np.pi * cfo_hz * t)
    corrected_samples = samples * cfo_correction
    
    symbols = []
    for i in range(8):  # נחלץ 8 סמלים
        start = pos + i * sps
        if start + sps > len(corrected_samples):
            break
            
        # חילוץ הסמל
        symbol_samples = corrected_samples[start:start + sps]
        
        # צמצום (decimation) - נקח דגימה כל 4
        decimated = symbol_samples[::4][:N]
        
        # מילוי אם נחסר
        if len(decimated) < N:
            decimated = np.pad(decimated, (0, N - len(decimated)))
        
        # FFT
        fft_result = np.fft.fft(decimated)
        
        # תיקון shift בגלל CFO
        if cfo_int != 0:
            fft_result = np.roll(fft_result, -cfo_int)
        
        # מציאת הפיק
        symbol_value = np.argmax(np.abs(fft_result))
        symbols.append(symbol_value)
    
    return symbols

def test_multiple_symbol_positions():
    """בדיקת מיקומים מרובים לחילוץ סמלים"""
    
    print("🧪 בדיקת מיקומים מרובים")
    print("=" * 40)
    
    # טעינת דגימות
    with open('temp/hello_world.cf32', 'rb') as f:
        data = f.read()
    
    samples = struct.unpack('<{}f'.format(len(data)//4), data)
    complex_samples = np.array([
        samples[i] + 1j*samples[i+1] 
        for i in range(0, len(samples), 2)
    ])
    
    print(f"📊 נטענו {len(complex_samples)} דגימות מורכבות")
    
    # קבלת מידע סנכרון
    sync_info = run_frame_sync()
    if not sync_info:
        return
    
    # חישוב מיקומים
    positions = calculate_precise_positions(sync_info)
    
    # הסמלים הצפויים מ-GNU Radio
    target_symbols = [9, 1, 1, 0, 27, 4, 26, 12]
    
    best_match = 0
    best_pos = None
    best_symbols = None
    
    print(f"\n🎯 בדיקת {len(positions)} מיקומים:")
    
    for pos in positions:
        if pos < 0 or pos + 8*512 > len(complex_samples):
            continue
            
        # חילוץ עם פרמטרים מדויקים
        symbols = extract_symbols_advanced(
            complex_samples, pos,
            sync_info.get('cfo_int', 9),
            sync_info.get('cfo_frac', 0.00164447)
        )
        
        # השוואה
        matches = sum(1 for i in range(min(8, len(symbols))) 
                     if symbols[i] == target_symbols[i])
        
        print(f"   מיקום {pos:5d}: {symbols[:4]}... התאמות: {matches}/8")
        
        if matches > best_match:
            best_match = matches
            best_pos = pos
            best_symbols = symbols
    
    if best_match > 0:
        print(f"\n🎉 הטוב ביותר: מיקום {best_pos} עם {best_match}/8 התאמות")
        print(f"   סמלים: {best_symbols}")
        print(f"   צפוי:   {target_symbols}")
        return best_pos, best_symbols
    else:
        print("\n😞 לא נמצאו התאמות טובות")
        return None, None

def try_gnu_radio_style_extraction():
    """ניסיון חילוץ בסגנון GNU Radio"""
    
    print("\n🔬 ניסיון חילוץ בסגנון GNU Radio")
    print("=" * 40)
    
    # נטען דגימות
    with open('temp/hello_world.cf32', 'rb') as f:
        data = f.read()
    
    samples = struct.unpack('<{}f'.format(len(data)//4), data)
    complex_samples = np.array([
        samples[i] + 1j*samples[i+1] 
        for i in range(0, len(samples), 2)
    ])
    
    # מיקום המוכר
    pos = 10976
    N = 128
    sps = 512
    
    print(f"📍 מיקום: {pos}")
    
    # חילוץ כמו ב-GNU Radio
    symbols_gnuradio_style = []
    
    for i in range(8):
        start = pos + i * sps
        symbol_data = complex_samples[start:start + sps]
        
        # GNU Radio style: נקח כל הסמל ונעשה FFT ישירות
        if len(symbol_data) >= N:
            # צמצום פשוט
            decimated = symbol_data[::4][:N]
            
            # FFT
            fft_vals = np.fft.fft(decimated)
            
            # לא roll - כמו GNU Radio
            symbol_val = np.argmax(np.abs(fft_vals))
            symbols_gnuradio_style.append(symbol_val)
    
    target = [9, 1, 1, 0, 27, 4, 26, 12]
    matches = sum(1 for i in range(8) if symbols_gnuradio_style[i] == target[i])
    
    print(f"GNU Radio style: {symbols_gnuradio_style}")
    print(f"Target:          {target}")
    print(f"התאמות: {matches}/8")
    
    return symbols_gnuradio_style

if __name__ == "__main__":
    os.chdir('/home/yakirqaq/projects/lora-lite-phy')
    
    print("🚀 חילוץ סמלים משופר")
    print("=" * 50)
    
    # בדיקת מיקומים מרובים
    best_pos, best_symbols = test_multiple_symbol_positions()
    
    # ניסיון סגנון GNU Radio
    gnu_symbols = try_gnu_radio_style_extraction()
    
    # סיכום
    print(f"\n📋 סיכום:")
    if best_pos:
        print(f"   הטוב ביותר: מיקום {best_pos}")
        print(f"   סמלים: {best_symbols}")
    print(f"   GNU Radio style: {gnu_symbols}")
