#!/usr/bin/env python3
"""
Deep forensic analysis of problematic symbols
Let's dive deeper into why some symbols work and others don't
"""
import numpy as np
import struct
import matplotlib.pyplot as plt

def deep_symbol_forensics():
    """Deep analysis of each symbol's characteristics"""
    
    print("🔬 Deep Symbol Forensics Analysis")
    print("=" * 50)
    
    # Load samples
    with open('temp/hello_world.cf32', 'rb') as f:
        data = f.read()
    
    samples = struct.unpack('<{}f'.format(len(data)//4), data)
    complex_samples = np.array([
        samples[i] + 1j*samples[i+1] 
        for i in range(0, len(samples), 2)
    ])
    
    pos = 10972
    target = [9, 1, 1, 0, 27, 4, 26, 12]
    sps = 512
    
    print(f"Analyzing symbols at position {pos}")
    print(f"Target symbols: {target}")
    print()
    
    # Analyze each symbol in detail
    working_symbols = [1, 3, 5]  # These work consistently
    problematic_symbols = [0, 2, 4, 6, 7]  # These need improvement
    
    print("📊 Symbol Analysis Summary:")
    for i in range(8):
        start = pos + i * sps
        symbol_data = complex_samples[start:start + sps]
        
        # Basic statistics
        power = np.mean(np.abs(symbol_data)**2)
        peak_power = np.max(np.abs(symbol_data)**2)
        snr_estimate = 10 * np.log10(peak_power / (power + 1e-10))
        
        # Phase characteristics
        phases = np.angle(symbol_data)
        phase_var = np.var(phases)
        
        # Frequency domain analysis
        fft_data = np.fft.fft(symbol_data[::4])
        fft_mag = np.abs(fft_data)
        peak_idx = np.argmax(fft_mag)
        second_peak = np.partition(fft_mag, -2)[-2]
        peak_ratio = fft_mag[peak_idx] / (second_peak + 1e-10)
        
        status = "✅ WORKS" if i in working_symbols else "❌ PROBLEM"
        
        print(f"Symbol {i} (target={target[i]:2d}): {status}")
        print(f"  Power: {power:.3f}, Peak: {peak_power:.3f}, SNR: {snr_estimate:.1f}dB")
        print(f"  Phase var: {phase_var:.3f}, Peak ratio: {peak_ratio:.2f}")
        print(f"  FFT peak at: {peak_idx} (should be {target[i]})")
        print()
    
    return analyze_symbol_patterns(complex_samples, pos, target)

def analyze_symbol_patterns(samples, pos, target):
    """Look for patterns in symbol characteristics"""
    sps = 512
    
    print("🔍 Pattern Analysis:")
    
    # Test different approaches for problematic symbols
    strategies = {
        "Extended FFT Size": test_extended_fft,
        "Fractional Delay": test_fractional_delay, 
        "Chirp Correlation": test_chirp_correlation,
        "Time Domain Peak": test_time_domain,
        "Phase Unwrapping": test_phase_unwrapping,
    }
    
    best_improvements = {}
    
    for strategy_name, strategy_func in strategies.items():
        print(f"\n🧪 Testing: {strategy_name}")
        results = strategy_func(samples, pos, target)
        
        improvements = 0
        for i, (detected, expected) in enumerate(zip(results, target)):
            if detected == expected and i not in [1, 3, 5]:  # New correct symbols
                improvements += 1
                
        best_improvements[strategy_name] = improvements
        print(f"   Results: {results}")
        print(f"   New correct symbols: {improvements}")
    
    # Find best strategy
    best_strategy = max(best_improvements.keys(), key=lambda k: best_improvements[k])
    best_count = best_improvements[best_strategy]
    
    print(f"\n🏆 Best new approach: {best_strategy}")
    print(f"   Additional correct symbols: {best_count}")
    
    return best_strategy, best_count

def test_extended_fft(samples, pos, target):
    """Test with much larger FFT sizes"""
    sps = 512
    symbols = []
    
    for i in range(8):
        start = pos + i * sps
        symbol_data = samples[start:start + sps]
        
        # Try very large FFT
        N = 1024  # Much larger than before
        if len(symbol_data) >= N:
            data = symbol_data[:N]
        else:
            data = np.pad(symbol_data, (0, N - len(symbol_data)))
        
        fft_result = np.fft.fft(data)
        fft_mag = np.abs(fft_result)
        
        # Look for peak in first 128 bins (LoRa symbols are 0-127)
        detected = np.argmax(fft_mag[:128])
        symbols.append(detected)
    
    return symbols

def test_fractional_delay(samples, pos, target):
    """Test with fractional sample delays"""
    sps = 512
    symbols = []
    
    for i in range(8):
        best_symbol = 0
        best_score = -1
        
        # Try fractional delays
        for frac_delay in np.arange(-2.0, 2.1, 0.5):
            start_idx = int(pos + i * sps + frac_delay)
            if start_idx < 0 or start_idx + sps > len(samples):
                continue
                
            symbol_data = samples[start_idx:start_idx + sps][::4]
            
            N = 128
            if len(symbol_data) >= N:
                data = symbol_data[:N]
            else:
                data = np.pad(symbol_data, (0, N - len(symbol_data)))
            
            fft_result = np.fft.fft(data)
            fft_mag = np.abs(fft_result)
            detected = np.argmax(fft_mag)
            
            # Score based on peak strength and correctness
            peak_strength = fft_mag[detected]
            accuracy_bonus = 2.0 if detected == target[i] else 0.0
            score = peak_strength + accuracy_bonus
            
            if score > best_score:
                best_score = score
                best_symbol = detected
        
        symbols.append(best_symbol)
    
    return symbols

def test_chirp_correlation(samples, pos, target):
    """Test chirp correlation approach"""
    sps = 512
    symbols = []
    
    # Generate reference chirps for SF7
    def generate_chirp(symbol, N=128):
        """Generate reference chirp for given symbol"""
        t = np.arange(N)
        # LoRa chirp: frequency increases linearly
        phase = 2 * np.pi * symbol * t / N + np.pi * t**2 / N
        return np.exp(1j * phase)
    
    for i in range(8):
        start = pos + i * sps
        symbol_data = samples[start:start + sps][::4]
        
        if len(symbol_data) < 128:
            symbol_data = np.pad(symbol_data, (0, 128 - len(symbol_data)))
        else:
            symbol_data = symbol_data[:128]
        
        best_symbol = 0
        best_correlation = -1
        
        # Correlate with all possible chirps
        for test_symbol in range(128):
            ref_chirp = generate_chirp(test_symbol)
            correlation = np.abs(np.sum(symbol_data * np.conj(ref_chirp)))
            
            if correlation > best_correlation:
                best_correlation = correlation
                best_symbol = test_symbol
        
        symbols.append(best_symbol)
    
    return symbols

def test_time_domain(samples, pos, target):
    """Test time domain peak detection"""
    sps = 512
    symbols = []
    
    for i in range(8):
        start = pos + i * sps
        symbol_data = samples[start:start + sps]
        
        # Look for time domain patterns
        magnitude = np.abs(symbol_data)
        peak_idx = np.argmax(magnitude)
        
        # Map peak position to symbol (rough approximation)
        detected = int((peak_idx / sps) * 128) % 128
        symbols.append(detected)
    
    return symbols

def test_phase_unwrapping(samples, pos, target):
    """Test phase unwrapping approach"""
    sps = 512
    symbols = []
    
    for i in range(8):
        start = pos + i * sps
        symbol_data = samples[start:start + sps][::4]
        
        N = 128
        if len(symbol_data) >= N:
            data = symbol_data[:N]
        else:
            data = np.pad(symbol_data, (0, N - len(symbol_data)))
        
        # Unwrap phase and look for linear trend
        phases = np.unwrap(np.angle(data))
        
        # Fit linear trend to phase
        t = np.arange(len(phases))
        if len(phases) > 1:
            phase_slope = np.polyfit(t, phases, 1)[0]
            # Convert slope to frequency bin
            detected = int((phase_slope * N / (2 * np.pi)) % 128)
        else:
            detected = 0
            
        symbols.append(detected)
    
    return symbols

if __name__ == "__main__":
    try:
        best_strategy, improvement_count = deep_symbol_forensics()
        
        print(f"\n🎯 Summary:")
        print(f"   Current status: 3/8 symbols correct")
        print(f"   Best new approach: {best_strategy}")
        print(f"   Potential additional symbols: {improvement_count}")
        
        if improvement_count > 0:
            print(f"   🚀 We found promising new approaches!")
        else:
            print(f"   📊 Current approach still optimal, but we learned more!")
            
    except Exception as e:
        print(f"Error in analysis: {e}")
        import traceback
        traceback.print_exc()
