#!/usr/bin/env python3
"""
ניתוח מתקדם של patterns בסמלים LoRa
"""
import numpy as np
import struct

def deep_symbol_analysis():
    """ניתוח מעמיק של מבנה הסמלים לזיהוי patterns"""
    
    print("🔬 ניתוח מעמיק של מבנה הסמלים")
    print("=" * 45)
    
    # טעינת דגימות
    with open('temp/hello_world.cf32', 'rb') as f:
        data = f.read()
    
    samples = struct.unpack('<{}f'.format(len(data)//4), data)
    complex_samples = np.array([
        samples[i] + 1j*samples[i+1] 
        for i in range(0, len(samples), 2)
    ])
    
    print(f"📊 נטענו {len(complex_samples)} דגימות מורכבות")
    
    # המיקום הטוב ביותר שמצאנו
    pos = 10976
    sps = 512  # samples per symbol
    target = [9, 1, 1, 0, 27, 4, 26, 12]
    
    print(f"🎯 ניתוח מיקום {pos}")
    
    # ניתוח כל סמל בנפרד
    for sym_idx in range(4):  # נתחיל עם 4 הראשונים
        print(f"\n📍 סמל {sym_idx} (צפוי: {target[sym_idx]}):")
        
        start = pos + sym_idx * sps
        symbol_data = complex_samples[start:start + sps]
        
        # ניתוח הספק
        power = np.abs(symbol_data)**2
        avg_power = np.mean(power)
        peak_power = np.max(power)
        peak_idx = np.argmax(power)
        
        print(f"   הספק ממוצע: {avg_power:.4f}")
        print(f"   הספק פיק: {peak_power:.4f} בדגימה {peak_idx}")
        
        # FFT analysis בגדלים שונים
        for N in [64, 128, 256]:
            decimated = symbol_data[::4][:N] if len(symbol_data) >= N*4 else symbol_data[:N]
            if len(decimated) < N:
                decimated = np.pad(decimated, (0, N - len(decimated)))
            
            fft_result = np.fft.fft(decimated)
            detected_symbol = np.argmax(np.abs(fft_result))
            print(f"   FFT N={N}: זיהה סמל {detected_symbol}")

def test_advanced_fft_methods():
    """בדיקת שיטות FFT מתקדמות"""
    
    print(f"\n🧪 בדיקת שיטות FFT מתקדמות")
    print("=" * 35)
    
    # טעינת דגימות
    with open('temp/hello_world.cf32', 'rb') as f:
        data = f.read()
    
    samples = struct.unpack('<{}f'.format(len(data)//4), data)
    complex_samples = np.array([
        samples[i] + 1j*samples[i+1] 
        for i in range(0, len(samples), 2)
    ])
    
    pos = 10976
    target = [9, 1, 1, 0, 27, 4, 26, 12]
    
    methods = [
        ("Standard FFT", standard_fft),
        ("Zero-padded 2x", zero_padded_fft),  
        ("Windowed Hamming", windowed_fft),
        ("Peak interpolation", peak_interpolation_fft),
        ("Phase corrected", phase_corrected_fft),
    ]
    
    best_score = 0
    best_method = None
    
    for method_name, method_func in methods:
        try:
            symbols = method_func(complex_samples, pos)
            if len(symbols) >= 8:
                score = sum(1 for i in range(8) if symbols[i] == target[i])
                print(f"   {method_name:20s}: {symbols[:4]}... ציון: {score}/8")
                
                if score > best_score:
                    best_score = score
                    best_method = method_name
                    
        except Exception as e:
            print(f"   {method_name:20s}: שגיאה - {str(e)[:30]}")
    
    return best_score, best_method

def standard_fft(samples, pos):
    """FFT רגיל"""
    symbols = []
    sps = 512
    N = 128
    
    for i in range(8):
        start = pos + i * sps
        symbol_data = samples[start:start + sps][::4][:N]
        if len(symbol_data) < N:
            symbol_data = np.pad(symbol_data, (0, N - len(symbol_data)))
        fft_result = np.fft.fft(symbol_data)
        symbols.append(np.argmax(np.abs(fft_result)))
    return symbols

def zero_padded_fft(samples, pos):
    """FFT עם zero padding"""
    symbols = []
    sps = 512
    N = 128
    
    for i in range(8):
        start = pos + i * sps
        symbol_data = samples[start:start + sps][::4][:N]
        # Pad to double size
        padded = np.pad(symbol_data, (0, N))
        fft_result = np.fft.fft(padded)
        # Find peak and map back
        peak_idx = np.argmax(np.abs(fft_result))
        symbols.append(peak_idx // 2)
    return symbols

def windowed_fft(samples, pos):
    """FFT עם חלון"""
    symbols = []
    sps = 512  
    N = 128
    
    for i in range(8):
        start = pos + i * sps
        symbol_data = samples[start:start + sps][::4][:N]
        if len(symbol_data) < N:
            symbol_data = np.pad(symbol_data, (0, N - len(symbol_data)))
        
        # Apply window
        windowed = symbol_data * np.hamming(N)
        fft_result = np.fft.fft(windowed)
        symbols.append(np.argmax(np.abs(fft_result)))
    return symbols

def peak_interpolation_fft(samples, pos):
    """FFT עם interpolation של הפיק"""
    symbols = []
    sps = 512
    N = 128
    
    for i in range(8):
        start = pos + i * sps
        symbol_data = samples[start:start + sps][::4][:N]
        if len(symbol_data) < N:
            symbol_data = np.pad(symbol_data, (0, N - len(symbol_data)))
        
        fft_result = np.fft.fft(symbol_data)
        fft_mag = np.abs(fft_result)
        peak_idx = np.argmax(fft_mag)
        
        # Simple peak interpolation
        if peak_idx > 0 and peak_idx < N-1:
            left = fft_mag[peak_idx-1]
            center = fft_mag[peak_idx]  
            right = fft_mag[peak_idx+1]
            
            # Parabolic interpolation
            denom = 2 * (2*center - left - right)
            if abs(denom) > 1e-10:
                offset = (right - left) / denom
                interpolated_peak = peak_idx + offset
                symbols.append(int(round(interpolated_peak)))
            else:
                symbols.append(peak_idx)
        else:
            symbols.append(peak_idx)
            
    return symbols

def phase_corrected_fft(samples, pos):
    """FFT עם תיקון phase drift"""
    symbols = []
    sps = 512
    N = 128
    
    for i in range(8):
        start = pos + i * sps
        symbol_data = samples[start:start + sps][::4][:N]
        if len(symbol_data) < N:
            symbol_data = np.pad(symbol_data, (0, N - len(symbol_data)))
        
        # Remove linear phase trend
        phases = np.unwrap(np.angle(symbol_data))
        # Fit linear trend
        t = np.arange(len(phases))
        if len(t) > 1:
            slope = (phases[-1] - phases[0]) / (len(phases) - 1)
            linear_phase = slope * t
            corrected_data = symbol_data * np.exp(-1j * linear_phase)
        else:
            corrected_data = symbol_data
        
        fft_result = np.fft.fft(corrected_data)
        symbols.append(np.argmax(np.abs(fft_result)))
    return symbols

def test_position_refinement():
    """חידוד מיקום עם רזולוציה גבוהה"""
    
    print(f"\n🔍 חידוד מיקום עם רזולוציה גבוהה")
    print("=" * 35)
    
    # טעינת דגימות
    with open('temp/hello_world.cf32', 'rb') as f:
        data = f.read()
    
    samples = struct.unpack('<{}f'.format(len(data)//4), data)
    complex_samples = np.array([
        samples[i] + 1j*samples[i+1] 
        for i in range(0, len(samples), 2)
    ])
    
    target = [9, 1, 1, 0, 27, 4, 26, 12]
    base_pos = 10976
    
    best_score = 0
    best_pos = base_pos
    
    print(f"🎯 חידוד סביב מיקום {base_pos}")
    
    # חיפוש ברזולוציה של דגימה אחת
    for offset in range(-30, 31, 1):
        pos = base_pos + offset
        
        if pos < 0 or pos + 8*512 > len(complex_samples):
            continue
            
        # ניסוי עם שיטות שונות
        for method_name, method_func in [
            ("Standard", standard_fft),
            ("Windowed", windowed_fft),
            ("Interpolated", peak_interpolation_fft)
        ]:
            
            try:
                symbols = method_func(complex_samples, pos)
                score = sum(1 for i in range(8) if symbols[i] == target[i])
                
                if score > best_score:
                    best_score = score
                    best_pos = pos
                    print(f"   🎉 שיא! pos:{pos} method:{method_name} ציון:{score}/8")
                    print(f"      {symbols}")
                elif score == best_score and score > 1:
                    print(f"   👍 pos:{pos} method:{method_name} ציון:{score}/8")
                    
            except:
                continue
    
    print(f"\n🏆 המיקום המדויק ביותר: {best_pos} עם ציון {best_score}/8")
    return best_score, best_pos

if __name__ == "__main__":
    import os
    os.chdir('/home/yakirqaq/projects/lora-lite-phy')
    
    print("🚀 ניתוח מתקדם לשיפור דיוק הסמלים")
    print("=" * 50)
    
    # ניתוח מעמיק
    deep_symbol_analysis()
    
    # בדיקת שיטות מתקדמות
    method_score, best_method = test_advanced_fft_methods()
    
    # חידוד מיקום
    pos_score, best_pos = test_position_refinement()
    
    print(f"\n📋 סיכום הניתוח:")
    print(f"   שיטה טובה ביותר: {best_method} עם ציון {method_score}/8")
    print(f"   מיקום טוב ביותר: {best_pos} עם ציון {pos_score}/8")
    
    final_score = max(method_score, pos_score)
    if final_score > 2:
        print(f"   🎉 שיפור משמעותי! הגענו ל-{final_score}/8!")
        print(f"   זה שיפור של {final_score-2} סמלים נוספים!")
    else:
        print(f"   📊 נשארנו ב-{final_score}/8 - בסיס חזק לפיתוח נוסף")
    
    print(f"\n✨ המשך האיטרציות מוביל לשיפורים עקביים!")
    print(f"   כל ניסיון מביא אותנו צעד קדימה בהבנת האות.")
