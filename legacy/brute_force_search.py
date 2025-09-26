#!/usr/bin/env python3
"""
חיפוש מקיף לפרמטרים הנכונים של חילוץ הסמלים
"""
import numpy as np
import struct

def brute_force_symbol_search():
    """חיפוש מקיף של הפרמטרים הנכונים"""
    
    print("🔍 חיפוש מקיף לפרמטרים נכונים")
    print("=" * 50)
    
    # טעינת דגימות
    with open('temp/hello_world.cf32', 'rb') as f:
        data = f.read()
    
    samples = struct.unpack('<{}f'.format(len(data)//4), data)
    complex_samples = np.array([
        samples[i] + 1j*samples[i+1] 
        for i in range(0, len(samples), 2)
    ])
    
    print(f"📊 נטענו {len(complex_samples)} דגימות מורכבות")
    
    target = [9, 1, 1, 0, 27, 4, 26, 12]
    best_score = 0
    best_config = None
    
    # פרמטרים לבדיקה
    positions = [4552, 8612, 10976, 12000, 15000, 20000]
    fft_sizes = [128, 64, 256]
    decimations = [4, 2, 8, 1]  # דילוג דגימות
    cfo_shifts = [0, -9, 9, -10, 10, -8, 8]
    
    total_tests = len(positions) * len(fft_sizes) * len(decimations) * len(cfo_shifts)
    current_test = 0
    
    print(f"🎯 אבדוק {total_tests} קומביניציות...")
    
    for pos in positions:
        for N in fft_sizes:
            for dec in decimations:
                for cfo_shift in cfo_shifts:
                    current_test += 1
                    
                    if current_test % 50 == 0:
                        progress = (current_test / total_tests) * 100
                        print(f"   📈 התקדמות: {progress:.1f}%")
                    
                    # בדיקה אם יש מספיק דגימות
                    if pos + 8 * 512 > len(complex_samples):
                        continue
                    
                    try:
                        symbols = extract_with_params(
                            complex_samples, pos, N, dec, cfo_shift
                        )
                        
                        if len(symbols) < 8:
                            continue
                            
                        score = sum(1 for i in range(8) 
                                  if symbols[i] == target[i])
                        
                        if score > best_score:
                            best_score = score
                            best_config = {
                                'position': pos,
                                'fft_size': N,
                                'decimation': dec,
                                'cfo_shift': cfo_shift,
                                'symbols': symbols,
                                'score': score
                            }
                            print(f"   🎉 שיא חדש! מיקום:{pos}, N:{N}, dec:{dec}, cfo:{cfo_shift} -> ציון:{score}/8")
                            print(f"      סמלים: {symbols[:8]}")
                            
                    except Exception as e:
                        continue
    
    print(f"\n🏆 התוצאה הטובה ביותר:")
    if best_config:
        for key, value in best_config.items():
            print(f"   {key}: {value}")
    else:
        print("   😞 לא נמצאה קומבינציה טובה")
    
    return best_config

def extract_with_params(samples, pos, N, decimation, cfo_shift):
    """חילוץ סמלים עם פרמטרים ניתנים להגדרה"""
    
    symbols = []
    sps = 512  # samples per symbol
    
    for i in range(8):
        start = pos + i * sps
        if start + sps > len(samples):
            break
            
        # קבלת הסמל
        symbol_data = samples[start:start + sps]
        
        # צמצום
        if decimation > 1:
            decimated = symbol_data[::decimation]
        else:
            decimated = symbol_data
            
        # התאמת אורך
        if len(decimated) >= N:
            decimated = decimated[:N]
        else:
            decimated = np.pad(decimated, (0, N - len(decimated)))
        
        # FFT
        fft_result = np.fft.fft(decimated)
        
        # תיקון CFO
        if cfo_shift != 0:
            fft_result = np.roll(fft_result, cfo_shift)
        
        # מציאת הפיק
        symbol_val = np.argmax(np.abs(fft_result))
        symbols.append(symbol_val)
    
    return symbols

def test_window_approach():
    """בדיקת גישה עם חלון חיפוש"""
    
    print("\n🔍 בדיקת גישת חלון חיפוש")
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
    
    # נבדוק חלון סביב המיקום המבטיח
    center_pos = 10976
    window = 2000
    step = 64
    
    best_score = 0
    best_pos = center_pos
    
    print(f"🎯 בדיקת חלון: {center_pos-window} עד {center_pos+window}")
    
    for pos in range(center_pos - window, center_pos + window, step):
        if pos < 0 or pos + 8*512 > len(complex_samples):
            continue
            
        # ננסה הפרמטרים הטובים ביותר שמצאנו
        symbols = extract_with_params(complex_samples, pos, 128, 4, 0)
        
        if len(symbols) >= 8:
            score = sum(1 for i in range(8) if symbols[i] == target[i])
            
            if score > 0:  # רק אם יש התאמה כלשהי
                print(f"   מיקום {pos:5d}: {symbols[:4]}... ציון: {score}/8")
                
            if score > best_score:
                best_score = score
                best_pos = pos
    
    print(f"הטוב ביותר בחלון: מיקום {best_pos} עם ציון {best_score}/8")

if __name__ == "__main__":
    import os
    os.chdir('/home/yakirqaq/projects/lora-lite-phy')
    
    print("🚀 חיפוש מקיף לפרמטרים נכונים")
    print("=" * 60)
    
    # חיפוש מקיף
    best_config = brute_force_symbol_search()
    
    # בדיקת חלון סביב מיקום מבטיח
    test_window_approach()
    
    print(f"\n📄 סיכום סופי:")
    print(f"   הציון הטוב ביותר שהשגנו: {best_config['score'] if best_config else 0}/8")
    if best_config:
        print(f"   הפרמטרים הטובים ביותר:")
        for key, value in best_config.items():
            if key != 'symbols':
                print(f"     {key}: {value}")
