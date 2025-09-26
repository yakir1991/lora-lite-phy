#!/usr/bin/env python3
"""
חזרה למיקום 10976 עם שיפורים ממוקדים
"""
import numpy as np
import struct

def focused_search_on_10976():
    """חיפוש ממוקד סביב מיקום 10976"""
    
    print("🎯 חיפוש ממוקד סביב מיקום 10976")
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
    
    base_pos = 10976
    
    # חיפוש עדין סביב 10976
    print(f"🔍 חיפוש עדין סביב {base_pos}")
    
    for offset in range(-100, 101, 4):  # ±100 דגימות בקפיצות של 4
        pos = base_pos + offset
        
        if pos < 0 or pos + 8*512 > len(complex_samples):
            continue
        
        # בדיקת גישות שונות
        approaches = [
            ("GNU Radio style", extract_gnu_radio_style),
            ("No CFO", lambda s, p: extract_no_cfo(s, p)),
            ("CFO -9", lambda s, p: extract_with_cfo(s, p, -9)),
            ("CFO +9", lambda s, p: extract_with_cfo(s, p, 9)),
            ("Decimation x2", lambda s, p: extract_decimation(s, p, 2)),
            ("Decimation x8", lambda s, p: extract_decimation(s, p, 8)),
        ]
        
        for approach_name, extract_func in approaches:
            try:
                symbols = extract_func(complex_samples, pos)
                if len(symbols) >= 8:
                    score = sum(1 for i in range(8) if symbols[i] == target[i])
                    
                    if score > best_score:
                        best_score = score
                        best_config = {
                            'position': pos,
                            'approach': approach_name,
                            'symbols': symbols[:8],
                            'score': score
                        }
                        print(f"   🎉 שיא חדש! pos:{pos}, {approach_name} -> {score}/8")
                        print(f"      סמלים: {symbols[:8]}")
                    elif score == best_score and score > 1:
                        print(f"   👍 תוצאה טובה: pos:{pos}, {approach_name} -> {score}/8")
                        
            except Exception as e:
                continue
    
    return best_config

def extract_gnu_radio_style(samples, pos):
    """חילוץ בסגנון GNU Radio מדויק"""
    symbols = []
    sps = 512
    N = 128
    
    for i in range(8):
        start = pos + i * sps
        if start + sps > len(samples):
            break
            
        # GNU Radio: decimation by 4, take first N samples
        symbol_data = samples[start:start + sps][::4][:N]
        
        if len(symbol_data) < N:
            symbol_data = np.pad(symbol_data, (0, N - len(symbol_data)))
            
        fft_result = np.fft.fft(symbol_data)
        symbol_val = np.argmax(np.abs(fft_result))
        symbols.append(symbol_val)
    
    return symbols

def extract_no_cfo(samples, pos):
    """חילוץ ללא תיקון CFO"""
    symbols = []
    sps = 512
    N = 128
    
    for i in range(8):
        start = pos + i * sps
        symbol_data = samples[start:start + sps][::4][:N]
        if len(symbol_data) < N:
            symbol_data = np.pad(symbol_data, (0, N - len(symbol_data)))
        fft_result = np.fft.fft(symbol_data)
        symbol_val = np.argmax(np.abs(fft_result))
        symbols.append(symbol_val)
    
    return symbols

def extract_with_cfo(samples, pos, cfo_shift):
    """חילוץ עם תיקון CFO"""
    symbols = []
    sps = 512
    N = 128
    
    for i in range(8):
        start = pos + i * sps
        symbol_data = samples[start:start + sps][::4][:N]
        if len(symbol_data) < N:
            symbol_data = np.pad(symbol_data, (0, N - len(symbol_data)))
        fft_result = np.fft.fft(symbol_data)
        fft_result = np.roll(fft_result, cfo_shift)
        symbol_val = np.argmax(np.abs(fft_result))
        symbols.append(symbol_val)
    
    return symbols

def extract_decimation(samples, pos, decimation):
    """חילוץ עם צמצום שונה"""
    symbols = []
    sps = 512
    N = 128
    
    for i in range(8):
        start = pos + i * sps
        symbol_data = samples[start:start + sps][::decimation][:N]
        if len(symbol_data) < N:
            symbol_data = np.pad(symbol_data, (0, N - len(symbol_data)))
        fft_result = np.fft.fft(symbol_data)
        symbol_val = np.argmax(np.abs(fft_result))
        symbols.append(symbol_val)
    
    return symbols
    samples = load_cf32_file('temp/hello_world.cf32')
    target = [9, 1, 1, 0, 27, 4, 26, 12]
    
    print(f"=== Brute Force Search ===")
    print(f"Searching for pattern: {target}")
    print(f"File has {len(samples)} samples")
    
    best_score = 0
    best_positions = []
    
    # Search every 32 samples from 0 to 20000
    step = 32
    end_search = min(20000, len(samples) - 8*512)
    
    for start_pos in range(0, end_search, step):
        symbols = extract_8_symbols(samples, start_pos)
        if not symbols:
            continue
            
        # Check matches
        matches = sum(1 for i in range(8) if symbols[i] == target[i])
        
        if matches > best_score:
            best_score = matches
            best_positions = [(start_pos, symbols)]
            print(f"NEW BEST: pos={start_pos}, matches={matches}/8, symbols={symbols}")
        elif matches == best_score and matches > 0:
            best_positions.append((start_pos, symbols))
            
        # Print every 100 steps to show progress
        if start_pos % (step * 100) == 0:
            print(f"Searched up to position {start_pos}...")
    
    print(f"\nFinal results: {len(best_positions)} positions with {best_score} matches")
    for pos, syms in best_positions[:5]:  # Show first 5
        print(f"  Position {pos}: {syms}")

def extract_8_symbols(samples, start_offset):
    """Extract 8 symbols using different demodulation approaches"""
    N = 128
    samples_per_symbol = 512
    
    if start_offset + 8 * samples_per_symbol > len(samples):
        return None
    
    symbols = []
    
    for i in range(8):
        sym_start = start_offset + i * samples_per_symbol
        sym_samples = samples[sym_start:sym_start + samples_per_symbol]
        
        # Method 1: Simple decimation + FFT
        decimated = sym_samples[::4][:N]
        fft_result = np.fft.fft(decimated)
        peak = np.argmax(np.abs(fft_result))
        
        symbols.append(peak)
    
    return symbols

if __name__ == "__main__":
    print("Starting brute force search (this may take a while)...")
def test_different_fft_windows():
    """בדיקת חלונות FFT שונים"""
    
    print("\n🔬 בדיקת חלונות FFT שונים")
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
    pos = 10976
    sps = 512
    
    # חלונות שונים
    windows = [
        ("Rectangular", None),
        ("Hanning", np.hanning),
        ("Hamming", np.hamming),
        ("Blackman", np.blackman),
    ]
    
    for window_name, window_func in windows:
        symbols = []
        
        for i in range(8):
            start = pos + i * sps
            symbol_data = complex_samples[start:start + sps][::4][:128]
            
            if len(symbol_data) < 128:
                symbol_data = np.pad(symbol_data, (0, 128 - len(symbol_data)))
            
            # החלת חלון
            if window_func is not None:
                window = window_func(len(symbol_data))
                symbol_data = symbol_data * window
            
            fft_result = np.fft.fft(symbol_data)
            symbol_val = np.argmax(np.abs(fft_result))
            symbols.append(symbol_val)
        
        score = sum(1 for i in range(8) if symbols[i] == target[i])
        print(f"   {window_name:12s}: {symbols[:4]}... ציון: {score}/8")

def verify_current_best():
    """אימות התוצאה הטובה הנוכחית"""
    
    print("\n✅ אימות התוצאה הטובה הנוכחית")
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
    pos = 10976
    
    # הגישה שנתנה לנו 2/8
    symbols = extract_gnu_radio_style(complex_samples, pos)
    score = sum(1 for i in range(8) if symbols[i] == target[i])
    
    print(f"מיקום {pos} עם GNU Radio style:")
    print(f"   סמלים: {symbols[:8]}")
    print(f"   צפוי:   {target}")
    print(f"   ציון:   {score}/8")
    
    # פירוט ההתאמות
    print("\nפירוט:")
    for i in range(8):
        match = "✅" if symbols[i] == target[i] else "❌"
        print(f"   סמל {i}: קיבלנו {symbols[i]:3d}, צפוי {target[i]:2d} {match}")

if __name__ == "__main__":
    import os
    os.chdir('/home/yakirqaq/projects/lora-lite-phy')
    
    print("🚀 התמקדות במיקום 10976")
    print("=" * 50)
    
    # אימות תוצאה נוכחית
    verify_current_best()
    
    # חיפוש ממוקד
    best_config = focused_search_on_10976()
    
    # בדיקת חלונות FFT
    test_different_fft_windows()
    
    if best_config:
        print(f"\n🏆 התוצאה הטובה ביותר:")
        print(f"   ציון: {best_config['score']}/8")
        print(f"   מיקום: {best_config['position']}")
        print(f"   גישה: {best_config['approach']}")
        print(f"   סמלים: {best_config['symbols']}")
    
    print(f"\n📈 הצעות לשיפור נוסף:")
    print(f"   1. בדיקת תיקון CFO עם ערכים עדינים יותר")
    print(f"   2. ניסיון עם FFT sizes שונים (64, 256)")
    print(f"   3. בדיקת מיקומים נוספים בטווח רחב יותר")
    print(f"   4. שילוב עם FrameSyncLite לקבלת מיקום מדויק יותר")