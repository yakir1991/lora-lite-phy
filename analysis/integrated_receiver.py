#!/usr/bin/env python3
"""
הטמעת receiver מלא עם שילוב C++ ו-Python לקבלת תוצאות מושלמות
"""
import os
import subprocess
import numpy as np
import struct

def run_full_cpp_analysis():
    """הרצת הניתוח המלא של C++"""
    
    print("🔧 הרצת ניתוח C++ מלא")
    print("=" * 30)
    
    # נריץ את כל הכלים שיש לנו
    cpp_tools = [
        ('./build_standalone/test_with_correct_sync', ['temp/hello_world.cf32', '7']),
        ('./build_standalone/test_receiver_lite', ['temp/hello_world.cf32']),
    ]
    
    results = {}
    
    for tool_path, args in cpp_tools:
        tool_name = os.path.basename(tool_path)
        print(f"🛠️  מריץ {tool_name}...")
        
        if not os.path.exists(tool_path):
            print(f"   ❌ לא קיים: {tool_path}")
            continue
            
        try:
            result = subprocess.run([tool_path] + args, 
                                  capture_output=True, text=True, timeout=10)
            
            results[tool_name] = {
                'returncode': result.returncode,
                'stdout': result.stdout,
                'stderr': result.stderr
            }
            
            print(f"   ✅ הושלם בהצלחה")
            
        except subprocess.TimeoutExpired:
            print(f"   ⏰ Timeout")
        except Exception as e:
            print(f"   ❌ שגיאה: {e}")
    
    return results

def extract_sync_parameters(cpp_results):
    """חילוץ פרמטרי סנכרון מתוצאות C++"""
    
    print("\n📊 חילוץ פרמטרי סנכרון")
    print("=" * 25)
    
    sync_params = {}
    
    # מ-test_with_correct_sync
    if 'test_with_correct_sync' in cpp_results:
        lines = cpp_results['test_with_correct_sync']['stdout'].split('\n')
        
        for line in lines:
            if '*** FRAME DETECTED! ***' in line:
                # Iteration 8: consumed=676, frame_detected=1, symbol_ready=0, snr_est=-16.9468 *** FRAME DETECTED! *** cfo_int=9, cfo_frac=0.00164447, sf=7
                sync_params['detection_iteration'] = 8
                parts = line.split()
                for part in parts:
                    if 'consumed=' in part:
                        sync_params['consumed_at_detection'] = int(part.split('=')[1].rstrip(','))
                    elif 'snr_est=' in part:
                        sync_params['snr'] = float(part.split('=')[1])
                    elif 'cfo_int=' in part:
                        sync_params['cfo_int'] = int(part.split('=')[1].rstrip(','))
                    elif 'cfo_frac=' in part:
                        sync_params['cfo_frac'] = float(part.split('=')[1].rstrip(','))
    
    # מ-test_receiver_lite
    if 'test_receiver_lite' in cpp_results:
        output = cpp_results['test_receiver_lite']['stdout']
        if 'payload_len=' in output:
            parts = output.split()
            for part in parts:
                if 'payload_len=' in part:
                    sync_params['payload_length'] = int(part.split('=')[1])
                elif 'ok=' in part:
                    sync_params['decode_ok'] = bool(int(part.split('=')[1]))
                elif 'crc=' in part:
                    sync_params['crc_ok'] = bool(int(part.split('=')[1]))
    
    print("פרמטרים שחולצו:")
    for key, value in sync_params.items():
        print(f"   {key}: {value}")
        
    return sync_params

def calculate_precise_symbol_position(sync_params):
    """חישוב מיקום מדויק של הסמלים על בסיס המידע מהסנכרון"""
    
    print(f"\n📍 חישוב מיקום מדויק של סמלים")
    print("=" * 32)
    
    if 'detection_iteration' not in sync_params:
        print("❌ חסר מידע על iteration של זיהוי")
        return None
    
    # חישוב מיקום הזיהוי
    iteration = sync_params['detection_iteration']
    
    # כל iteration צורכת דגימות שונות
    samples_consumed = 0
    for i in range(iteration):
        if i == 3:
            samples_consumed += 292
        elif i == 8:
            samples_consumed += 676
        else:
            samples_consumed += 512
    
    print(f"זיהוי frame ב-iteration {iteration}, דגימה {samples_consumed}")
    
    # LoRa frame structure:
    # Preamble: 8 symbols of SF7 = 8 * 128 samples * oversample(4) = 4096
    # SFD/Sync: 2.25 symbols = 2.25 * 128 * 4 = 1152 samples
    # Header starts immediately after
    
    oversample = 4
    samples_per_symbol = (2**7) * oversample  # 128 * 4 = 512
    
    preamble_samples = 8 * samples_per_symbol  # 4096
    sfd_samples = int(2.25 * samples_per_symbol)  # 1152
    
    # מיקום תחילת הנתונים (header)
    data_start = samples_consumed + preamble_samples + sfd_samples
    
    print(f"תחילת נתונים משוערת: דגימה {data_start}")
    
    # יצירת רשימת מיקומים לבדיקה
    candidate_positions = [
        data_start,
        data_start - 512,  # symbol offset
        data_start + 512,
        data_start - 256,  # half symbol offset
        data_start + 256,
        samples_consumed + 1000,  # הנחה שונה על המבנה
        samples_consumed + 2000,
        10976,  # המיקום שעובד חלקית
        4432,   # מיקום אחר שמצאנו
    ]
    
    return candidate_positions

def test_advanced_demodulation(positions, sync_params):
    """בדיקת demodulation מתקדם עם המידע מהסנכרון"""
    
    print(f"\n🧪 בדיקת demodulation מתקדם")
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
    best_score = 0
    best_result = None
    
    # פרמטרי CFO מהסנכרון
    cfo_int = sync_params.get('cfo_int', 9)
    cfo_frac = sync_params.get('cfo_frac', 0.00164447)
    
    print(f"CFO parameters: int={cfo_int}, frac={cfo_frac}")
    
    for pos in positions:
        if pos < 0 or pos + 8*512 > len(complex_samples):
            continue
        
        # דרכים שונות לעשות demodulation
        methods = [
            ("Basic FFT", lambda: extract_basic_fft(complex_samples, pos)),
            ("With CFO correction", lambda: extract_with_cfo_correction(
                complex_samples, pos, cfo_int, cfo_frac)),
            ("GNU Radio style", lambda: extract_gnu_radio_exact(complex_samples, pos)),
            ("Phase tracking", lambda: extract_with_phase_tracking(complex_samples, pos)),
        ]
        
        for method_name, extract_func in methods:
            try:
                symbols = extract_func()
                if len(symbols) >= 8:
                    score = sum(1 for i in range(8) if symbols[i] == target[i])
                    
                    if score > best_score:
                        best_score = score
                        best_result = {
                            'position': pos,
                            'method': method_name,
                            'symbols': symbols[:8],
                            'score': score
                        }
                        print(f"   🎉 שיא חדש! pos:{pos}, {method_name} -> {score}/8")
                        print(f"      {symbols[:8]}")
                    elif score == best_score and score > 0:
                        print(f"   👍 תוצאה טובה: pos:{pos}, {method_name} -> {score}/8")
                        
            except Exception as e:
                continue
    
    return best_result

def extract_basic_fft(samples, pos):
    """חילוץ FFT בסיסי"""
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

def extract_with_cfo_correction(samples, pos, cfo_int, cfo_frac):
    """חילוץ עם תיקון CFO מדויק"""
    symbols = []
    sps = 512
    N = 128
    fs = 500000  # sample rate
    
    # חישוב CFO ב-Hz
    cfo_hz = cfo_int * (fs / (2**7)) + cfo_frac * fs
    
    # יצירת correction vector
    t = np.arange(len(samples)) / fs
    cfo_correction = np.exp(-1j * 2 * np.pi * cfo_hz * t)
    
    # החלת תיקון CFO
    corrected_samples = samples * cfo_correction
    
    for i in range(8):
        start = pos + i * sps
        symbol_data = corrected_samples[start:start + sps][::4][:N]
        if len(symbol_data) < N:
            symbol_data = np.pad(symbol_data, (0, N - len(symbol_data)))
        fft_result = np.fft.fft(symbol_data)
        symbols.append(np.argmax(np.abs(fft_result)))
    return symbols

def extract_gnu_radio_exact(samples, pos):
    """חילוץ מדויק כמו GNU Radio"""
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

def extract_with_phase_tracking(samples, pos):
    """חילוץ עם מעקב phase"""
    symbols = []
    sps = 512
    N = 128
    
    prev_phase = 0
    
    for i in range(8):
        start = pos + i * sps
        symbol_data = samples[start:start + sps][::4][:N]
        if len(symbol_data) < N:
            symbol_data = np.pad(symbol_data, (0, N - len(symbol_data)))
        
        # תיקון phase drift
        avg_phase = np.angle(np.mean(symbol_data))
        phase_diff = avg_phase - prev_phase
        symbol_data = symbol_data * np.exp(-1j * phase_diff)
        prev_phase = avg_phase
        
        fft_result = np.fft.fft(symbol_data)
        symbols.append(np.argmax(np.abs(fft_result)))
    return symbols

if __name__ == "__main__":
    os.chdir('/home/yakirqaq/projects/lora-lite-phy')
    
    print("🚀 Receiver מלא עם שילוב C++ ו-Python")
    print("=" * 50)
    
    # הרצת ניתוח C++
    cpp_results = run_full_cpp_analysis()
    
    # חילוץ פרמטרי סנכרון
    sync_params = extract_sync_parameters(cpp_results)
    
    # חישוב מיקומים מדויקים
    positions = calculate_precise_symbol_position(sync_params)
    
    if positions:
        # בדיקת demodulation מתקדם
        best_result = test_advanced_demodulation(positions, sync_params)
        
        print(f"\n🏆 התוצאה הטובה ביותר:")
        if best_result:
            print(f"   ציון: {best_result['score']}/8")
            print(f"   מיקום: {best_result['position']}")
            print(f"   שיטה: {best_result['method']}")
            print(f"   סמלים: {best_result['symbols']}")
            print(f"   צפוי:   {[9, 1, 1, 0, 27, 4, 26, 12]}")
        else:
            print("   לא נמצאה תוצאה טובה")
    else:
        print("❌ לא ניתן לחשב מיקומים")
