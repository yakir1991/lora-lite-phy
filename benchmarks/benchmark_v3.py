#!/usr/bin/env python3
"""
LoRa Receiver V3 Benchmark Test
Compare V3 with original receiver
"""

import subprocess
import os
import json
import time

def run_receiver(receiver_script, file_path):
    """Run a receiver and parse its output"""
    try:
        cmd = ["python3", receiver_script, file_path]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        
        if result.returncode == 0:
            output_lines = result.stdout.split('\n')
            
            symbols = None
            confidence = 0.0
            position = None
            
            for line in output_lines:
                # Look for symbols in different formats
                if ('Raw symbols:' in line or 'Symbols:' in line or 'raw_symbols' in line) and '[' in line:
                    start = line.find('[')
                    end = line.find(']')
                    if start != -1 and end != -1:
                        symbols_str = line[start+1:end]
                        try:
                            symbols = [int(x.strip()) for x in symbols_str.split(',') if x.strip()]
                        except:
                            pass
                
                if 'Confidence:' in line and '%' in line:
                    conf_part = line.split('Confidence:')[1].strip().replace('%', '')
                    try:
                        confidence = float(conf_part) / 100.0
                    except:
                        confidence = 0.0
                
                if 'Frame Position:' in line or 'frame position:' in line.lower():
                    if 'Frame Position:' in line:
                        pos_part = line.split('Frame Position:')[1].strip()
                    else:
                        pos_part = line.split(':')[1].strip()
                    try:
                        position = int(pos_part) if pos_part != 'N/A' else None
                    except:
                        position = None
            
            return {
                'symbols': symbols or [],
                'confidence': confidence,
                'position': position,
                'success': symbols is not None,
                'error': None
            }
        else:
            return {'symbols': [], 'confidence': 0.0, 'position': None, 'success': False, 'error': result.stderr}
    
    except Exception as e:
        return {'symbols': [], 'confidence': 0.0, 'position': None, 'success': False, 'error': str(e)}

def compare_results(original, v3, expected_symbols):
    """Compare results between receivers"""
    
    # Position accuracy
    position_match = "✅" if original.get('position') == v3.get('position') else "❌"
    
    # Symbol accuracy
    orig_symbols = original.get('symbols', [])
    v3_symbols = v3.get('symbols', [])
    
    symbols_match = "✅" if orig_symbols == v3_symbols else "❌"
    
    # Expected symbols match
    orig_vs_expected = "✅" if orig_symbols == expected_symbols else "❌"
    v3_vs_expected = "✅" if v3_symbols == expected_symbols else "❌"
    
    return {
        'position_match': position_match,
        'symbols_match': symbols_match,
        'original_vs_expected': orig_vs_expected,
        'v3_vs_expected': v3_vs_expected
    }

def main():
    print("🎯 LORA RECEIVER V3.0 BENCHMARK")
    print("=" * 50)
    
    test_vectors = [
        {
            'file': 'temp/hello_world.cf32',
            'name': 'Hello World',
            'expected_symbols': [9, 1, 53, 0, 20, 4, 72, 12]
        }
    ]
    
    receivers = {
        'Original': 'complete_lora_receiver.py',
        'V3 Enhanced': 'lora_receiver_v3.py'
    }
    
    for test_vector in test_vectors:
        file_path = test_vector['file']
        test_name = test_vector['name']
        expected_symbols = test_vector['expected_symbols']
        
        if not os.path.exists(file_path):
            print(f"❌ Test file not found: {file_path}")
            continue
        
        print(f"\n📁 Testing: {test_name}")
        print(f"   File: {file_path}")
        print(f"   Expected symbols: {expected_symbols}")
        print("-" * 50)
        
        results = {}
        
        for receiver_name, receiver_script in receivers.items():
            print(f"🔧 Running {receiver_name}...")
            
            start_time = time.time()
            result = run_receiver(receiver_script, file_path)
            end_time = time.time()
            
            processing_time = end_time - start_time
            
            success = "✅" if result['success'] else "❌"
            symbols = result.get('symbols', [])
            confidence = result.get('confidence', 0.0)
            position = result.get('position', 'N/A')
            
            print(f"   Status: {success}")
            print(f"   Position: {position}")
            print(f"   Symbols: {symbols}")
            print(f"   Confidence: {confidence:.1%}")
            print(f"   Time: {processing_time:.2f}s")
            
            # Check against expected
            if symbols == expected_symbols:
                print(f"   Accuracy: ✅ Perfect match!")
            else:
                print(f"   Accuracy: ❌ Different from expected")
            
            results[receiver_name] = result
            results[receiver_name]['processing_time'] = processing_time
            
            print()
        
        # Compare results
        if len(results) == 2:
            original_result = results['Original']
            v3_result = results['V3 Enhanced']
            
            comparison = compare_results(original_result, v3_result, expected_symbols)
            
            print("🔍 COMPARISON ANALYSIS:")
            print(f"   Position match: {comparison['position_match']}")
            print(f"   Symbols match: {comparison['symbols_match']}")
            print(f"   Original vs Expected: {comparison['original_vs_expected']}")
            print(f"   V3 vs Expected: {comparison['v3_vs_expected']}")
            
            # Performance comparison
            orig_time = original_result.get('processing_time', 0)
            v3_time = v3_result.get('processing_time', 0)
            
            if orig_time > 0:
                speedup = orig_time / v3_time if v3_time > 0 else 0
                print(f"   Speed comparison: V3 is {speedup:.2f}x {'faster' if speedup > 1 else 'slower'}")
            
            # Overall assessment
            if (comparison['symbols_match'] == "✅" and 
                comparison['v3_vs_expected'] == "✅"):
                print("🎉 V3 MATCHES ORIGINAL PERFECTLY!")
            else:
                print("⚠️  V3 has differences from original")

if __name__ == "__main__":
    main()
