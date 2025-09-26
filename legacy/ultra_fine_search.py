#!/usr/bin/env python3
"""
שיפור נוסף סביב התוצאה הטובה ביותר
"""
import numpy as np
import struct

def ultra_fine_search():
    """חיפוש אולטרה עדין סביב התוצאה הטובה"""
    
    print("🔎 חיפוש אולטרה עדין")
    print("=" * 30)
    
    # טעינת דגימות
    with open('temp/hello_world.cf32', 'rb') as f:
        data = f.read()
    
    samples = struct.unpack('<{}f'.format(len(data)//4), data)
    complex_samples = np.array([
        samples[i] + 1j*samples[i+1] 
        for i in range(0, len(samples), 2)
    ])
    
    target = [9, 1, 1, 0, 27, 4, 26, 12]
    best_score = 3  # התחלנו מהכי טוב שיש לנו
    best_config = None
    
    # פרמטרים סביב התוצאה הטובה ביותר
    base_pos = 4432
    positions = range(base_pos - 50, base_pos + 50, 4)  # חיפוש בדיוק של 4 דגימות
    fft_sizes = [60, 62, 64, 66, 68]  # סביב 64
    cfo_shifts = [7, 8, 9]  # סביב 8
    
    print(f"🎯 חיפוש סביב pos={base_pos}, N=64, cfo=8")
    
    for pos in positions:
        for N in fft_sizes:
            for cfo in cfo_shifts:
                if pos + 8 * 512 > len(complex_samples):
                    continue
                
                try:
                    symbols = extract_symbol_accurate(complex_samples, pos, N, cfo)
                    
                    if len(symbols) >= 8:
                        score = sum(1 for i in range(8) 
                                  if symbols[i] == target[i])
                        
                        if score >= best_score:
                            if score > best_score:
                                print(f"   🎉 שיא חדש! pos:{pos}, N:{N}, cfo:{cfo} -> {score}/8")
                                print(f"      סמלים: {symbols[:8]}")
                                print(f"      צפוי:   {target}")
                                best_score = score
                                best_config = {
                                    'position': pos,
                                    'fft_size': N,
                                    'cfo_shift': cfo,
                                    'symbols': symbols[:8],
                                    'score': score
                                }
                            elif score == best_score:
                                print(f"   👍 תוצאה טובה: pos:{pos}, N:{N}, cfo:{cfo} -> {score}/8")
                                print(f"      סמלים: {symbols[:8]}")
                            
                except Exception as e:
                    continue
    
    return best_config

def extract_symbol_accurate(samples, pos, N, cfo_shift):
    """חילוץ סמלים מדויק"""
    
    symbols = []
    sps = 512
    
    for i in range(8):
        start = pos + i * sps
        if start + sps > len(samples):
            break
            
        # קח את תחילת הסמל (הכי יציב)
        symbol_data = samples[start:start + N]
        
        if len(symbol_data) < N:
            symbol_data = np.pad(symbol_data, (0, N - len(symbol_data)))
        
        # FFT עם תיקון CFO
        fft_result = np.fft.fft(symbol_data)
        if cfo_shift != 0:
            fft_result = np.roll(fft_result, cfo_shift)
        
        symbol_val = np.argmax(np.abs(fft_result))
        symbols.append(symbol_val)
    
    return symbols

def analyze_best_result():
    """ניתוח מפורט של התוצאה הטובה"""
    
    print("\n📊 ניתוח מפורט של התוצאה הטובה")
    print("=" * 40)
    
    # טעינת דגימות
    with open('temp/hello_world.cf32', 'rb') as f:
        data = f.read()
    
    samples = struct.unpack('<{}f'.format(len(data)//4), data)
    complex_samples = np.array([
        samples[i] + 1j*samples[i+1] 
        for i in range(0, len(samples), 2)
    ])
    
    # הפרמטרים הטובים ביותר
    pos = 4432
    N = 64
    cfo = 8
    target = [9, 1, 1, 0, 27, 4, 26, 12]
    
    symbols = extract_symbol_accurate(complex_samples, pos, N, cfo)
    
    print(f"מיקום: {pos}, FFT size: {N}, CFO shift: {cfo}")
    print(f"סמלים:  {symbols[:8]}")
    print(f"צפוי:    {target}")
    print(f"התאמות: {sum(1 for i in range(8) if symbols[i] == target[i])}/8")
    
    print("\nפירוט ההתאמות:")
    for i in range(8):
        match = "✅" if symbols[i] == target[i] else "❌"
        print(f"   סמל {i}: קיבלנו {symbols[i]:2d}, צפוי {target[i]:2d} {match}")
    
    # בדיקת הספק של כל סמל
    print("\nניתוח הספק:")
    sps = 512
    for i in range(4):  # רק הסמלים הראשונים
        start = pos + i * sps
        symbol_data = complex_samples[start:start + N]
        power = np.mean(np.abs(symbol_data)**2)
        print(f"   סמל {i}: הספק ממוצע = {power:.4f}")

def test_gnu_radio_comparison():
    """השוואה עם גישת GNU Radio"""
    
    print("\n🔬 השוואה עם GNU Radio")
    print("=" * 25)
    
    # טעינת דגימות
    with open('temp/hello_world.cf32', 'rb') as f:
        data = f.read()
    
    samples = struct.unpack('<{}f'.format(len(data)//4), data)
    complex_samples = np.array([
        samples[i] + 1j*samples[i+1] 
        for i in range(0, len(samples), 2)
    ])
    
    target = [9, 1, 1, 0, 27, 4, 26, 12]
    
    # הגישה שלנו (הטובה ביותר)
    pos_our = 4432
    symbols_our = extract_symbol_accurate(complex_samples, pos_our, 64, 8)
    score_our = sum(1 for i in range(8) if symbols_our[i] == target[i])
    
    # גישת GNU Radio (מיקום 10976)
    pos_gnu = 10976  
    symbols_gnu = extract_gnu_style(complex_samples, pos_gnu)
    score_gnu = sum(1 for i in range(8) if symbols_gnu[i] == target[i])
    
    print(f"הגישה שלנו   (pos {pos_our}): {symbols_our[:8]} ציון: {score_our}/8")
    print(f"גישת GNU Radio (pos {pos_gnu}): {symbols_gnu[:8]} ציון: {score_gnu}/8")
    print(f"צפוי:                          {target}")

def extract_gnu_style(samples, pos):
    """חילוץ בסגנון GNU Radio"""
    symbols = []
    sps = 512
    N = 128
    
    for i in range(8):
        start = pos + i * sps
        symbol_data = samples[start:start + sps][::4][:N]  # decimation by 4
        fft_result = np.fft.fft(symbol_data)
        symbol_val = np.argmax(np.abs(fft_result))
        symbols.append(symbol_val)
    
    return symbols

if __name__ == "__main__":
    import os
    os.chdir('/home/yakirqaq/projects/lora-lite-phy')
    
    print("🚀 שיפור אולטרה עדין לחילוץ סמלים")
    print("=" * 50)
    
    # חיפוש אולטרה עדין
    best_config = ultra_fine_search()
    
    # ניתוח התוצאה הטובה
    analyze_best_result()
    
    # השוואה עם GNU Radio
    test_gnu_radio_comparison()
    
    if best_config:
        print(f"\n🏆 התוצאה הסופית הטובה ביותר:")
        print(f"   ציון: {best_config['score']}/8")
        print(f"   מיקום: {best_config['position']}")
        print(f"   FFT size: {best_config['fft_size']}")
        print(f"   CFO shift: {best_config['cfo_shift']}")
    else:
        print(f"\n📊 נשארנו עם התוצאה הטובה: 3/8 התאמות")
