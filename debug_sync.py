#!/usr/bin/env python3
"""
משתמש ב-ReceiverLite של C++ לקבלת הסמלים הנכונים
"""

import subprocess
import os
import numpy as np

def test_cpp_receiver():
    """בדיקה של ReceiverLite החזק"""
    
    print("🔧 בדיקת ReceiverLite C++")
    print("=" * 40)
    
    # נבדוק אם יש לנו executable של test_receiver_lite
    cpp_tests = [
        './build_standalone/test_receiver_lite',
        './build_standalone/test_full_chain_parity', 
        './build_standalone/run_vector',
    ]
    
    for test_exe in cpp_tests:
        print(f"🧪 בודק: {test_exe}")
        
        if not os.path.exists(test_exe):
            print(f"   ❌ לא קיים: {test_exe}")
            continue
            
        try:
            # ננסה להריץ עם הקובץ שלנו
            result = subprocess.run([
                test_exe, 'temp/hello_world.cf32'
            ], capture_output=True, text=True, timeout=10)
            
            print(f"   📤 Exit code: {result.returncode}")
            
            if result.stdout:
                print(f"   📄 Output:")
                # נציג רק שורות מעניינות
                lines = result.stdout.split('\n')
                for line in lines:
                    if any(keyword in line.lower() for keyword in 
                          ['symbol', 'payload', 'hello', 'decoded', 'error', 'success']):
                        print(f"      {line}")
            
            if result.stderr:
                print(f"   ⚠️  Errors:")
                print(f"      {result.stderr[:200]}...")
                
        except subprocess.TimeoutExpired:
            print(f"   ⏰ Timeout")
        except Exception as e:
            print(f"   ❌ Error: {e}")
        
        print()

def build_improved_symbol_extractor():
    """בניית מחלץ סמלים משופר"""
    
    print("🛠️  בניית מחלץ סמלים משופר")
    print("=" * 40)
    
    # נכין גרסה C++ שמדפיסה סמלים
    cpp_code = """
#include <iostream>
#include <vector>
#include <complex>
#include <fstream>
#include "include/receiver_lite.hpp"

using namespace lora_lite;

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <cf32_file>" << std::endl;
        return 1;
    }

    // Load samples
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<std::complex<float>> samples;
    
    float real, imag;
    while (file.read(reinterpret_cast<char*>(&real), sizeof(float)) &&
           file.read(reinterpret_cast<char*>(&imag), sizeof(float))) {
        samples.push_back(std::complex<float>(real, imag));
    }
    
    std::cout << "הוטענו " << samples.size() << " דגימות" << std::endl;
    
    // Setup receiver
    ReceiverParams params;
    params.sf = 7;
    params.oversample = 4;
    params.sync_words = {93, 112};  // הערכים שמצאנו
    
    ReceiverLite receiver(params);
    
    // Process in chunks
    const size_t chunk_size = 1024;
    std::vector<uint8_t> payload;
    
    for (size_t i = 0; i < samples.size(); i += chunk_size) {
        size_t end = std::min(i + chunk_size, samples.size());
        std::vector<std::complex<float>> chunk(samples.begin() + i, samples.begin() + end);
        
        auto result = receiver.process_samples(chunk);
        
        if (result.frame_detected) {
            std::cout << "🎯 Frame detected at sample " << i << std::endl;
        }
        
        if (!result.payload.empty()) {
            std::cout << "📦 Payload received (" << result.payload.size() << " bytes): ";
            for (auto byte : result.payload) {
                std::cout << std::hex << (int)byte << " ";
            }
            std::cout << std::endl;
            
            // Try to decode as ASCII
            std::string ascii_payload;
            for (auto byte : result.payload) {
                if (byte >= 32 && byte <= 126) {
                    ascii_payload += (char)byte;
                } else {
                    ascii_payload += '.';
                }
            }
            std::cout << "📜 ASCII: '" << ascii_payload << "'" << std::endl;
            
            payload = result.payload;
            break;
        }
    }
    
    if (payload.empty()) {
        std::cout << "❌ לא נמצא payload" << std::endl;
    }
    
    return 0;
}
"""
    
    # כתיבת הקובץ
    with open('debug_receiver_test.cpp', 'w') as f:
        f.write(cpp_code)
    
    print("✅ נוצר: debug_receiver_test.cpp")
    
    # נסה לקמפל
    try:
        result = subprocess.run([
            'g++', '-std=c++17', '-I.', 
            'debug_receiver_test.cpp', 
            '-L./build_standalone', '-llora_lite',
            '-o', 'debug_receiver_test'
        ], capture_output=True, text=True)
        
        if result.returncode == 0:
            print("✅ הקומפילציה הצליחה")
            return True
        else:
            print(f"❌ שגיאת קומפילציה:")
            print(result.stderr)
            return False
            
    except Exception as e:
        print(f"❌ שגיאה: {e}")
        return False

def analyze_symbol_timing():
    """ניתוח מדויק יותר של הטיימינג"""
    
    print("⏰ ניתוח טיימינג מדויק")
    print("=" * 30)
    
    # נשתמש במידע שיש לנו מה-C++
    frame_detection_info = {
        'iteration': 8,
        'total_consumed': 8136,
        'cfo_int': 9,
        'cfo_frac': 0.00164447,
        'snr': -16.9468
    }
    
    print("📊 מידע מזיהוי הפריים:")
    for key, value in frame_detection_info.items():
        print(f"   {key}: {value}")
    
    # חישוב מדויק של המיקום
    # האיטרציות הן: 0-7: 512 כל אחת, 3: 292, 8: 676
    samples_before_8 = 512 * 7 + 292  # iterations 0-7
    detection_sample = samples_before_8  # כאן זוהה הפריים
    
    print(f"\n📍 מיקום זיהוי הפריים: דגימה {detection_sample}")
    
    # LoRa frame structure:
    # Preamble: 8 symbols = 8 * 512 = 4096 samples  
    # Sync word: 2.25 symbols = 1152 samples
    # Header starts after that
    
    preamble_length = 8 * 512  # 4096
    sync_length = int(2.25 * 512)  # 1152
    
    # אם זיהינו בדגימה 3876, הנתונים אמורים להתחיל אחרי הפרהמבל + sync
    header_start = detection_sample + preamble_length + sync_length
    
    print(f"📦 תחילת Header משוערת: דגימה {header_start}")
    
    # בואו ננסה מיקומים שונים סביב זה
    test_positions = [
        header_start,
        header_start - 512,
        header_start + 512, 
        header_start - 256,
        header_start + 256,
        10976,  # המיקום הטוב שמצאנו
        4552,   # המיקום שחישבנו קודם
    ]
    
    return test_positions

def test_multiple_approaches(positions):
    """בדיקת גישות מרובות לחילוץ סמלים"""
    
    print("🧪 בדיקת גישות מרובות")
    print("=" * 30)
    
    import struct
    
    # טעינת הדגימות
    with open('temp/hello_world.cf32', 'rb') as f:
        data = f.read()
    samples = struct.unpack('<{}f'.format(len(data)//4), data)
    complex_samples = np.array([samples[i] + 1j*samples[i+1] for i in range(0, len(samples), 2)])
    
    target = [9, 1, 1, 0, 27, 4, 26, 12]
    
    for pos in positions:
        if pos < 0 or pos + 8*512 > len(complex_samples):
            continue
            
        print(f"\n🎯 בדיקת מיקום {pos}:")
        
        # גישה 1: עם תיקון CFO מדויק
        symbols1 = extract_with_precise_cfo(complex_samples, pos)
        matches1 = sum(1 for i in range(8) if symbols1[i] == target[i])
        print(f"   CFO מדויק: {symbols1[:4]}... - התאמות: {matches1}/8")
        
        # גישה 2: בלי CFO
        symbols2 = extract_without_cfo(complex_samples, pos)
        matches2 = sum(1 for i in range(8) if symbols2[i] == target[i])
        print(f"   בלי CFO: {symbols2[:4]}... - התאמות: {matches2}/8")
        
        # גישה 3: עם שלבי צטמצטות שונים
        symbols3 = extract_with_different_decimation(complex_samples, pos)
        matches3 = sum(1 for i in range(8) if symbols3[i] == target[i])
        print(f"   צמצום שונה: {symbols3[:4]}... - התאמות: {matches3}/8")
        
        best_score = max(matches1, matches2, matches3)
        if best_score > 0:
            print(f"   🎉 מיקום מבטיח! ציון מקסימלי: {best_score}/8")

def extract_with_precise_cfo(samples, pos):
    N, sps, fs = 128, 512, 500000
    cfo_hz = 8790.7
    
    # תיקון CFO
    t = np.arange(len(samples)) / fs
    correction = np.exp(-1j * 2 * np.pi * cfo_hz * t)
    samples = samples * correction
    
    symbols = []
    for i in range(8):
        start = pos + i * sps
        sym = samples[start:start + sps][::4][:N]
        fft_result = np.roll(np.fft.fft(sym), -9)
        symbols.append(np.argmax(np.abs(fft_result)))
    return symbols

def extract_without_cfo(samples, pos):
    N, sps = 128, 512
    symbols = []
    for i in range(8):
        start = pos + i * sps
        sym = samples[start:start + sps][::4][:N]
        fft_result = np.fft.fft(sym)
        symbols.append(np.argmax(np.abs(fft_result)))
    return symbols

def extract_with_different_decimation(samples, pos):
    N, sps = 128, 512
    symbols = []
    for i in range(8):
        start = pos + i * sps
        sym = samples[start:start + sps]
        # צמצום שונה - נקח את האמצע
        decimated = sym[sps//8::sps//N][:N]
        fft_result = np.fft.fft(decimated)
        symbols.append(np.argmax(np.abs(fft_result)))
    return symbols

if __name__ == "__main__":
    os.chdir('/home/yakirqaq/projects/lora-lite-phy')
    
    print("🚀 שיפור חילוץ הסמלים")
    print("=" * 50)
    
    # בדיקת כלי C++ קיימים
    test_cpp_receiver()
    
    # ניתוח טיימינג מדויק
    positions = analyze_symbol_timing()
    
    # בדיקת גישות מרובות
    test_multiple_approaches(positions)
    
    # ניסיון בניית כלי C++ חדש
    if build_improved_symbol_extractor():
        print("\n🎯 מריץ כלי C++ חדש:")
        try:
            result = subprocess.run(['./debug_receiver_test', 'temp/hello_world.cf32'], 
                                  capture_output=True, text=True, timeout=10)
            print(result.stdout)
            if result.stderr:
                print(f"Errors: {result.stderr}")
        except Exception as e:
            print(f"שגיאה בהרצת הכלי החדש: {e}")
