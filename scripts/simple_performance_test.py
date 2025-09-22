#!/usr/bin/env python3
"""
Simple performance test for individual vectors
"""

import subprocess
import time
import os
import json

def test_single_vector(vector_path, config_name):
    """Test a single vector and return performance data"""
    print(f"\n=== Testing {config_name} ===")
    print(f"Vector: {vector_path}")
    
    if not os.path.exists(vector_path):
        print(f"Vector file not found: {vector_path}")
        return None
    
    # Get file size
    file_size = os.path.getsize(vector_path)
    print(f"File size: {file_size:,} bytes")
    
    # Run scheduler with specific vector
    start_time = time.time()
    try:
        result = subprocess.run(
            ['./build/test_gr_pipeline', vector_path],
            capture_output=True,
            text=True,
            timeout=60
        )
        end_time = time.time()
        
        if result.returncode != 0:
            print(f"Error running scheduler: {result.stderr}")
            return None
            
        # Parse output for performance metrics
        output = result.stdout
        lines = output.split('\n')
        
        # Find the last occurrence of each metric
        performance_data = {
            'config_name': config_name,
            'vector_path': vector_path,
            'file_size_bytes': file_size,
            'execution_time_sec': end_time - start_time,
            'samples_per_sec': 0,
            'frames_per_sec': 0,
            'total_frames': 0,
            'total_samples': 0,
            'preamble_detections': 0,
            'header_decodes': 0,
            'payload_decodes': 0,
            'success_rate': 0.0
        }
        
        # Look for the specific test results
        for line in reversed(lines):
            if '[DEBUG] Performance:' in line and performance_data['samples_per_sec'] == 0:
                # Extract: [DEBUG] Performance: 1.64 MSamples/sec, 650.96 frames/sec, 2341145 μs total
                parts = line.split(':')[1].strip().split(',')
                for part in parts:
                    part = part.strip()
                    if 'MSamples/sec' in part:
                        samples_per_sec = float(part.split()[0]) * 1e6
                        performance_data['samples_per_sec'] = samples_per_sec
                    elif 'frames/sec' in part:
                        performance_data['frames_per_sec'] = float(part.split()[0])
                    elif 'μs total' in part:
                        performance_data['execution_time_sec'] = float(part.split()[0]) / 1e6
            
            elif '[DEBUG] Operations:' in line and performance_data['preamble_detections'] == 0:
                # Extract: [DEBUG] Operations: 1524 preamble detections, 1524 header decodes, 1524 payload decodes
                parts = line.split(':')[1].strip().split(',')
                for part in parts:
                    part = part.strip()
                    if 'preamble detections' in part:
                        performance_data['preamble_detections'] = int(part.split()[0])
                    elif 'header decodes' in part:
                        performance_data['header_decodes'] = int(part.split()[0])
                    elif 'payload decodes' in part:
                        performance_data['payload_decodes'] = int(part.split()[0])
            
            elif '[DEBUG] Pipeline completed:' in line and performance_data['total_samples'] == 0:
                # Extract: [DEBUG] Pipeline completed: processed 3833388/3833388 samples, found 1524 frames
                parts = line.split(':')[1].strip().split(',')
                for part in parts:
                    part = part.strip()
                    if 'processed' in part and 'samples' in part:
                        samples_part = part.split()[1].split('/')[0]
                        performance_data['total_samples'] = int(samples_part)
                    elif 'found' in part and 'frames' in part:
                        performance_data['total_frames'] = int(part.split()[1])
        
        # Calculate success rate
        if performance_data['total_frames'] > 0:
            performance_data['success_rate'] = performance_data['payload_decodes'] / performance_data['total_frames']
        
        print(f"Results: {performance_data['samples_per_sec']:,.0f} samples/sec, {performance_data['frames_per_sec']:.1f} frames/sec, {performance_data['total_frames']} frames")
        return performance_data
        
    except subprocess.TimeoutExpired:
        print("Test timed out after 60 seconds")
        return None
    except Exception as e:
        print(f"Error running test: {e}")
        return None

def main():
    """Test individual vectors"""
    print("LoRa Scheduler Individual Vector Performance Test")
    print("="*60)
    
    # Test vectors
    test_vectors = [
        {
            'path': 'vectors/sps_125k_bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false_nmsgs_8.unknown',
            'name': 'SF7, 125kHz, CR45, 8 frames'
        },
        {
            'path': 'vectors/sps_1M_bw_250k_sf_8_cr_3_ldro_true_crc_true_implheader_false_test_message.unknown',
            'name': 'SF8, 250kHz, CR47, LDRO'
        },
        {
            'path': 'vectors/sps_500k_bw_125k_sf_7_cr_2_ldro_false_crc_true_implheader_false_hello_stupid_world.unknown',
            'name': 'SF7, 500kHz, CR46, Hello World'
        }
    ]
    
    results = []
    
    for test_vector in test_vectors:
        result = test_single_vector(test_vector['path'], test_vector['name'])
        if result:
            results.append(result)
    
    if results:
        print("\n" + "="*60)
        print("PERFORMANCE SUMMARY")
        print("="*60)
        
        print(f"\n{'Config':<30} {'Samples/sec':<15} {'Frames/sec':<12} {'Frames':<8} {'Success Rate':<12}")
        print("-" * 80)
        
        for result in results:
            print(f"{result['config_name']:<30} "
                  f"{result['samples_per_sec']:>12,.0f} "
                  f"{result['frames_per_sec']:>10.1f} "
                  f"{result['total_frames']:>6} "
                  f"{result['success_rate']:>10.1%}")
        
        # Save results
        with open('individual_performance_results.json', 'w') as f:
            json.dump(results, f, indent=2)
        print(f"\nResults saved to: individual_performance_results.json")
        
        # Calculate averages
        avg_samples = sum(r['samples_per_sec'] for r in results) / len(results)
        avg_frames = sum(r['frames_per_sec'] for r in results) / len(results)
        total_frames = sum(r['total_frames'] for r in results)
        
        print(f"\nAverage Performance:")
        print(f"  Samples/sec: {avg_samples:,.0f}")
        print(f"  Frames/sec: {avg_frames:.1f}")
        print(f"  Total frames processed: {total_frames}")
        
        # Compare with GNU Radio
        gr_samples = 500000
        gr_frames = 50
        improvement_samples = avg_samples / gr_samples
        improvement_frames = avg_frames / gr_frames
        
        print(f"\nvs GNU Radio LoRa SDR:")
        print(f"  Sample processing: {improvement_samples:.1f}x faster")
        print(f"  Frame processing: {improvement_frames:.1f}x faster")
    else:
        print("No test results obtained")

if __name__ == '__main__':
    main()
