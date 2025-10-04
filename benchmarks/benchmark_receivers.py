#!/usr/bin/env python3
"""
Advanced LoRa Receiver Benchmark System
Compare accuracy between different receiver versions
"""

import json
import subprocess
import os
from pathlib import Path
import numpy as np
from typing import Dict, List, Any
import time

class LoRaBenchmark:
    """Benchmark system for LoRa receivers"""
    
    def __init__(self):
        # Define receivers to test
        self.receivers = {
            'original': self.run_original_receiver,
            'advanced_v2': self.run_advanced_receiver,
            'v3_enhanced': self.run_v3_receiver,
        }
        
        self.test_vectors = [
            {'file': 'temp/hello_world.cf32', 'expected_position': 10972, 'name': 'hello_world'},
            {'file': 'temp/long_message.cf32', 'expected_position': 1000, 'name': 'long_message'},
            {'file': 'golden_vectors_demo/tx_sf7_bw125000_cr2_crc1_impl0_ldro2_pay11.cf32', 'expected_position': 2500, 'name': 'golden_vector'}
        ]
        
        self.results = {}
    
    def run_v3_receiver(self, file_path):
        """Test V3 enhanced receiver"""
        try:
            cmd = ["python3", "lora_receiver_v3.py", file_path]
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
            
            if result.returncode == 0:
                # Parse output for symbols
                output_lines = result.stdout.split('\n')
                
                symbols = None
                confidence = 0.0
                position = None
                
                for line in output_lines:
                    if 'Symbols:' in line and '[' in line:
                        # Extract symbols from "Symbols: [9, 1, 53, 0, 20, 4, 72, 12]"
                        start = line.find('[')
                        end = line.find(']')
                        if start != -1 and end != -1:
                            symbols_str = line[start+1:end]
                            symbols = [int(x.strip()) for x in symbols_str.split(',')]
                    
                    if 'Confidence:' in line and '%' in line:
                        # Extract confidence from "Confidence: 85.27%"
                        conf_part = line.split('Confidence:')[1].strip().replace('%', '')
                        confidence = float(conf_part) / 100.0
                    
                    if 'Frame Position:' in line:
                        # Extract position from "Frame Position: 10972"
                        pos_part = line.split('Frame Position:')[1].strip()
                        position = int(pos_part) if pos_part != 'N/A' else None
                
                return {
                    'symbols': symbols or [],
                    'confidence': confidence,
                    'frame_position': position,
                    'success': symbols is not None,
                    'error': None
                }
            else:
                return {'symbols': [], 'confidence': 0.0, 'frame_position': None, 'success': False, 'error': result.stderr}
        
        except Exception as e:
            return {'symbols': [], 'confidence': 0.0, 'frame_position': None, 'success': False, 'error': str(e)}
    try:
        cmd = ["python3", "lora_receiver_v3.py", file_path]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        
        if result.returncode == 0:
            # Parse output for symbols
            output_lines = result.stdout.split('
')
            
            symbols = None
            confidence = 0.0
            position = None
            
            for line in output_lines:
                if 'Symbols:' in line and '[' in line:
                    # Extract symbols from "Symbols: [9, 1, 53, 0, 20, 4, 72, 12]"
                    start = line.find('[')
                    end = line.find(']')
                    if start != -1 and end != -1:
                        symbols_str = line[start+1:end]
                        symbols = [int(x.strip()) for x in symbols_str.split(',')]
                
                if 'Confidence:' in line and '%' in line:
                    # Extract confidence from "Confidence: 85.27%"
                    conf_part = line.split('Confidence:')[1].strip().replace('%', '')
                    confidence = float(conf_part) / 100.0
                
                if 'Frame Position:' in line:
                    # Extract position from "Frame Position: 10972"
                    pos_part = line.split('Frame Position:')[1].strip()
                    position = int(pos_part) if pos_part != 'N/A' else None
            
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
    
    def calculate_accuracy_metrics(self, result: Dict[str, Any], expected: Dict[str, Any]) -> Dict[str, float]:
        """Calculate accuracy metrics"""
        metrics = {
            'position_accuracy': 0.0,
            'symbol_accuracy': 0.0,
            'overall_score': 0.0
        }
        
        if not result.get('success', False):
            return metrics
        
        # Position accuracy
        if result.get('frame_position') is not None and expected.get('expected_position') is not None:
            position_error = abs(result['frame_position'] - expected['expected_position'])
            max_error = expected['expected_position'] * 0.1  # 10% tolerance
            if position_error <= max_error:
                metrics['position_accuracy'] = 1.0 - (position_error / max_error)
            else:
                metrics['position_accuracy'] = 0.0
        
        # Symbol accuracy (if we have reference symbols)
        if result.get('symbols') and expected.get('reference_symbols'):
            ref_symbols = expected['reference_symbols']
            test_symbols = result['symbols']
            
            if len(ref_symbols) == len(test_symbols):
                correct_symbols = sum(1 for i in range(len(ref_symbols)) if ref_symbols[i] == test_symbols[i])
                metrics['symbol_accuracy'] = correct_symbols / len(ref_symbols)
            else:
                metrics['symbol_accuracy'] = 0.0
        else:
            # If no reference, use confidence as proxy
            metrics['symbol_accuracy'] = result.get('confidence', 0.0)
        
        # Overall score
        confidence_weight = 0.3
        position_weight = 0.4
        symbol_weight = 0.3
        
        metrics['overall_score'] = (
            (result.get('confidence', 0.0) * confidence_weight) +
            (metrics['position_accuracy'] * position_weight) +
            (metrics['symbol_accuracy'] * symbol_weight)
        )
        
        return metrics
    
    def run_benchmark(self) -> Dict[str, Any]:
        """Run complete benchmark"""
        
        print("🚀 LORA RECEIVER BENCHMARK")
        print("=" * 50)
        
        benchmark_results = {}
        
        # Add reference symbols for known vectors
        test_vectors_with_ref = self.test_vectors.copy()
        test_vectors_with_ref[0]['reference_symbols'] = [9, 1, 53, 0, 20, 4, 72, 12]  # hello_world known good
        
        for receiver_name in self.receivers.keys():
            print(f"\n🔧 Testing receiver: {receiver_name}")
            print("-" * 30)
            
            receiver_results = []
            
            for test_vector in test_vectors_with_ref:
                test_file = test_vector['file']
                test_name = test_vector['name']
                
                print(f"📁 Testing: {test_name}")
                
                if not os.path.exists(f"/home/yakirqaq/projects/lora-lite-phy/{test_file}"):
                    print(f"   ⚠️ File not found: {test_file}")
                    continue
                
                # Run receiver
                result = self.run_receiver(receiver_name, test_file)
                
                # Calculate metrics
                metrics = self.calculate_accuracy_metrics(result, test_vector)
                
                # Store results
                test_result = {
                    'test_name': test_name,
                    'test_file': test_file,
                    'result': result,
                    'metrics': metrics
                }
                
                receiver_results.append(test_result)
                
                # Print results
                if result['success']:
                    print(f"   ✅ Success")
                    print(f"   📍 Position: {result.get('frame_position', 'N/A')} (accuracy: {metrics['position_accuracy']:.2%})")
                    print(f"   🎯 Symbol accuracy: {metrics['symbol_accuracy']:.2%}")
                    print(f"   📊 Overall score: {metrics['overall_score']:.2%}")
                    print(f"   ⏱️ Processing time: {result['processing_time']:.2f}s")
                else:
                    print(f"   ❌ Failed: {result.get('error', 'Unknown error')}")
            
            benchmark_results[receiver_name] = receiver_results
        
        # Calculate summary statistics
        summary = self.calculate_summary(benchmark_results)
        
        return {
            'detailed_results': benchmark_results,
            'summary': summary
        }
    
    def calculate_summary(self, benchmark_results: Dict) -> Dict[str, Any]:
        """Calculate summary statistics"""
        
        summary = {}
        
        for receiver_name, results in benchmark_results.items():
            successful_tests = [r for r in results if r['result']['success']]
            
            if successful_tests:
                avg_position_accuracy = np.mean([r['metrics']['position_accuracy'] for r in successful_tests])
                avg_symbol_accuracy = np.mean([r['metrics']['symbol_accuracy'] for r in successful_tests])
                avg_overall_score = np.mean([r['metrics']['overall_score'] for r in successful_tests])
                avg_processing_time = np.mean([r['result']['processing_time'] for r in successful_tests])
                success_rate = len(successful_tests) / len(results)
                
                summary[receiver_name] = {
                    'success_rate': success_rate,
                    'avg_position_accuracy': avg_position_accuracy,
                    'avg_symbol_accuracy': avg_symbol_accuracy,
                    'avg_overall_score': avg_overall_score,
                    'avg_processing_time': avg_processing_time,
                    'total_tests': len(results)
                }
            else:
                summary[receiver_name] = {
                    'success_rate': 0.0,
                    'avg_position_accuracy': 0.0,
                    'avg_symbol_accuracy': 0.0,
                    'avg_overall_score': 0.0,
                    'avg_processing_time': 0.0,
                    'total_tests': len(results)
                }
        
        return summary
    
    def print_summary(self, benchmark_results: Dict[str, Any]):
        """Print benchmark summary"""
        
        summary = benchmark_results['summary']
        
        print("\n" + "="*60)
        print("📊 BENCHMARK SUMMARY")
        print("="*60)
        
        for receiver_name, stats in summary.items():
            print(f"\n🔧 {receiver_name.upper()}:")
            print(f"   Success Rate: {stats['success_rate']:.2%}")
            print(f"   Position Accuracy: {stats['avg_position_accuracy']:.2%}")
            print(f"   Symbol Accuracy: {stats['avg_symbol_accuracy']:.2%}")
            print(f"   Overall Score: {stats['avg_overall_score']:.2%}")
            print(f"   Avg Processing Time: {stats['avg_processing_time']:.2f}s")
        
        # Comparison
        if len(summary) >= 2:
            receivers = list(summary.keys())
            r1, r2 = receivers[0], receivers[1]
            
            print(f"\n🔍 COMPARISON ({r1} vs {r2}):")
            
            improvements = {
                'Success Rate': summary[r2]['success_rate'] - summary[r1]['success_rate'],
                'Position Accuracy': summary[r2]['avg_position_accuracy'] - summary[r1]['avg_position_accuracy'],
                'Symbol Accuracy': summary[r2]['avg_symbol_accuracy'] - summary[r1]['avg_symbol_accuracy'],
                'Overall Score': summary[r2]['avg_overall_score'] - summary[r1]['avg_overall_score']
            }
            
            for metric, improvement in improvements.items():
                status = "📈" if improvement > 0 else "📉" if improvement < 0 else "➡️"
                print(f"   {status} {metric}: {improvement:+.2%}")
    
    def save_results(self, benchmark_results: Dict[str, Any], filename: str = "benchmark_results.json"):
        """Save benchmark results to file"""
        try:
            with open(filename, 'w') as f:
                json.dump(benchmark_results, f, indent=2, default=str)
            print(f"\n📁 Benchmark results saved to: {filename}")
        except Exception as e:
            print(f"⚠️ Could not save results: {e}")


def main():
    benchmark = LoRaBenchmark()
    results = benchmark.run_benchmark()
    benchmark.print_summary(results)
    benchmark.save_results(results)


if __name__ == "__main__":
    main()
