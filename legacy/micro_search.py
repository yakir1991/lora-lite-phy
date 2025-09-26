#!/usr/bin/env python3
"""
חיפוש מיקרו סביב הפרמטרים הטובים שמצאנו
"""
import numpy as np
import struct

def micro_search():
    """חיפוש מיקרו סביב הפרמטרים הטובים"""
    
    print("🔬 חיפוש מיקרו לשיפור התוצאות")
    print("=" * 40)
    
    # טעינת דגימות
    with open('temp/hello_world.cf32', 'rb') as f:
        data = f.read()
    
    samples = struct.unpack('<{}f'.format(len(data)//4), data)
    complex_samples = np.array([
        samples[i] + 1j*samples[i+1] 
        for i in range(0, len(samples), 2)
    ])
    
    target = [9, 1, 1, 0, 27, 4, 26, 12]
    best_score = 0
    best_config = None
    
    # הפרמטרים הטובים שמצאנו
    base_pos = 4552
    base_N = 64
    base_cfo = 8
    
    # חיפוש מיקרו
    positions = range(base_pos - 200, base_pos + 200, 8)  # חיפוש עדין יותר
    fft_sizes = [32, 64, 96, 128, 160]  # סביב 64
    cfo_shifts = [6, 7, 8, 9, 10]  # סביב 8
    
    total_tests = len(positions) * len(fft_sizes) * len(cfo_shifts)
    current_test = 0
    
    print(f"🎯 אבדוק {total_tests} קומביניציות מיקרו...")
    
    for pos in positions:
        for N in fft_sizes:
            for cfo_shift in cfo_shifts:
                current_test += 1
                
                if current_test % 100 == 0:
                    progress = (current_test / total_tests) * 100
                    print(f"   📈 התקדמות: {progress:.1f}%")
                
                if pos + 8 * 512 > len(complex_samples):
                    continue
                
                try:
                    symbols = extract_fine_tuned(
                        complex_samples, pos, N, cfo_shift
                    )
                    
                    if len(symbols) >= 8:
                        score = sum(1 for i in range(8) 
                                  if symbols[i] == target[i])
                        
                        if score > best_score:
                            best_score = score
                            best_config = {
                                'position': pos,
                                'fft_size': N,
                                'cfo_shift': cfo_shift,
                                'symbols': symbols[:8],
                                'score': score
                            }
                            print(f"   🎉 שיא חדש! pos:{pos}, N:{N}, cfo:{cfo_shift} -> {score}/8")
                            print(f"      סמלים: {symbols[:8]}")
                            
                except Exception as e:
                    continue
    
    return best_config

def extract_fine_tuned(samples, pos, N, cfo_shift):
    """חילוץ סמלים מכוונן עדין"""
    
    symbols = []
    sps = 512  # samples per symbol
    
    for i in range(8):
        start = pos + i * sps
        if start + sps > len(samples):
            break
            
        # קבלת כל הסמל (בלי צמצום)
        symbol_data = samples[start:start + sps]
        
        # השתמש בחלק הטוב ביותר של הסמל
        # LoRa symbol צריך להיות יציב, אז נקח את האמצע
        quarter = len(symbol_data) // 4
        symbol_core = symbol_data[quarter:quarter + N]
        
        if len(symbol_core) < N:
            symbol_core = np.pad(symbol_core, (0, N - len(symbol_core)))
        elif len(symbol_core) > N:
            symbol_core = symbol_core[:N]
        
        # FFT
        fft_result = np.fft.fft(symbol_core)
        
        # תיקון CFO
        if cfo_shift != 0:
            fft_result = np.roll(fft_result, cfo_shift)
        
        # מציאת הפיק
        symbol_val = np.argmax(np.abs(fft_result))
        symbols.append(symbol_val)
    
    return symbols

def test_different_regions():
    """בדיקת אזורים שונים בסמל"""
    
    print("\n🧪 בדיקת אזורים שונים בסמל")
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
    pos = 4552
    N = 64
    cfo = 8
    sps = 512
    
    print(f"🎯 בדיקת אזורים שונים במיקום {pos}, N={N}, CFO={cfo}")
    
    regions = [
        ("התחלה", 0, N),
        ("רבע ראשון", sps//4, N), 
        ("אמצע", sps//2 - N//2, N),
        ("רבע שלישי", 3*sps//4, N),
        ("סוף", sps-N, N),
    ]
    
    for region_name, offset, length in regions:
        symbols = []
        
        for i in range(8):
            start = pos + i * sps + offset
            if start + length > len(complex_samples):
                continue
                
            symbol_data = complex_samples[start:start + length]
            fft_result = np.roll(np.fft.fft(symbol_data), cfo)
            symbol_val = np.argmax(np.abs(fft_result))
            symbols.append(symbol_val)
        
        if len(symbols) >= 8:
            score = sum(1 for i in range(8) if symbols[i] == target[i])
            print(f"   {region_name:12s}: {symbols[:4]}... ציון: {score}/8")
            if score > 2:
                print(f"      🎉 שיא חדש! {symbols}")

if __name__ == "__main__":
    import os
    os.chdir('/home/yakirqaq/projects/lora-lite-phy')
    
    print("🚀 חיפוש מיקרו לשיפור דיוק הסמלים")
    print("=" * 50)
    
    # חיפוש מיקרו
    best_config = micro_search()
    
    # בדיקת אזורים שונים בסמל
    test_different_regions()
    
    print(f"\n📊 תוצאות מיקרו:")
    if best_config:
        print(f"   ציון: {best_config['score']}/8")
        print(f"   פרמטרים: pos={best_config['position']}, N={best_config['fft_size']}, cfo={best_config['cfo_shift']}")
        print(f"   סמלים: {best_config['symbols']}")
        print(f"   צפוי:   {[9, 1, 1, 0, 27, 4, 26, 12]}")
    else:
        print("   לא נמצא שיפור")
