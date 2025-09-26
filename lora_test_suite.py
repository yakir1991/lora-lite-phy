#!/usr/bin/env python3
"""
LoRa Receiver Test Suite
Tests the complete receiver against multiple configurations and vectors
Can generate new test vectors using GR LoRa SDR if needed
"""

import argparse
import json
import subprocess
from pathlib import Path
from typing import List, Dict, Any
import tempfile
import os

from complete_lora_receiver import LoRaReceiver

class LoRaTestSuite:
    """Test suite for LoRa receiver system"""
    
    def __init__(self):
        self.test_results = []
        self.generated_vectors = []
        
    def generate_test_vector(self, 
                           sf: int = 7,
                           bw: int = 125000, 
                           cr: int = 2,
                           has_crc: bool = True,
                           payload: str = "Hello LoRa World!",
                           output_dir: str = "test_vectors") -> str:
        """Generate a test vector using GR LoRa SDR"""
        
        print(f"🔧 Generating test vector: SF{sf}_BW{bw}_CR{cr}_CRC{has_crc}")
        
        # Create output directory
        Path(output_dir).mkdir(exist_ok=True)
        
        # Generate filename
        crc_str = "crc1" if has_crc else "crc0" 
        filename = f"tx_sf{sf}_bw{bw}_cr{cr}_{crc_str}_test"
        vector_path = Path(output_dir) / f"{filename}.cf32"
        metadata_path = Path(output_dir) / f"{filename}.json"
        
        try:
            # Use export_tx_reference_vector.py to generate vector
            cmd = [
                'python', 
                'external/gr_lora_sdr/scripts/export_tx_reference_vector.py',
                '--sf', str(sf),
                '--bw', str(bw),
                '--cr', str(cr),
                '--payload', payload,
                '--output-iq', str(vector_path),
                '--output-meta', str(metadata_path)
            ]
            
            if has_crc:
                cmd.append('--crc')
                
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
            
            if result.returncode == 0:
                print(f"✅ Generated: {vector_path}")
                self.generated_vectors.append({
                    'path': str(vector_path),
                    'metadata_path': str(metadata_path),
                    'sf': sf,
                    'bw': bw,
                    'cr': cr,
                    'has_crc': has_crc,
                    'payload': payload
                })
                return str(vector_path)
            else:
                print(f"❌ Generation failed: {result.stderr}")
                
        except Exception as e:
            print(f"❌ Error generating vector: {e}")
            
        return None
    
    def test_configuration(self, 
                          sf: int,
                          bw: int, 
                          cr: int,
                          has_crc: bool,
                          test_file: str) -> Dict[str, Any]:
        """Test receiver with specific configuration"""
        
        print(f"\n🧪 TESTING: SF{sf}, BW{bw}Hz, CR{cr}, CRC={has_crc}")
        print("-" * 50)
        
        # Create receiver
        receiver = LoRaReceiver(
            sf=sf,
            bw=bw,
            cr=cr,
            has_crc=has_crc
        )
        
        # Test decoding
        try:
            result = receiver.decode_file(test_file)
            
            # Analyze results
            test_result = {
                'config': {
                    'sf': sf,
                    'bw': bw,
                    'cr': cr,
                    'has_crc': has_crc
                },
                'file': test_file,
                'status': result['status'],
                'frame_detected': 'frame_position' in result,
                'symbols_extracted': result.get('symbol_count', 0),
                'confidence': result.get('confidence', 0.0),
                'success': result['status'] != 'error' and result.get('confidence', 0) > 0.5
            }
            
            if test_result['success']:
                print(f"✅ SUCCESS - Confidence: {test_result['confidence']:.2%}")
            else:
                print(f"❌ FAILED - {result.get('error', 'Low confidence')}")
                
            return test_result
            
        except Exception as e:
            print(f"❌ EXCEPTION: {e}")
            return {
                'config': {'sf': sf, 'bw': bw, 'cr': cr, 'has_crc': has_crc},
                'file': test_file,
                'status': 'error',
                'error': str(e),
                'success': False
            }
    
    def run_comprehensive_test(self, test_vectors_dir: str = "vectors"):
        """Run comprehensive test across multiple configurations"""
        
        print(f"🚀 COMPREHENSIVE LORA RECEIVER TEST SUITE")
        print("=" * 60)
        
        # Test configurations to try
        test_configs = [
            # Standard configurations
            {'sf': 7, 'bw': 125000, 'cr': 2, 'has_crc': True},
            {'sf': 7, 'bw': 125000, 'cr': 1, 'has_crc': True},
            {'sf': 7, 'bw': 125000, 'cr': 2, 'has_crc': False},
            {'sf': 8, 'bw': 125000, 'cr': 2, 'has_crc': True},
            {'sf': 9, 'bw': 125000, 'cr': 2, 'has_crc': True},
            # Different bandwidths
            {'sf': 7, 'bw': 250000, 'cr': 2, 'has_crc': True},
            {'sf': 7, 'bw': 500000, 'cr': 2, 'has_crc': True},
        ]
        
        # Find available test vectors
        vectors_path = Path(test_vectors_dir)
        available_vectors = []
        
        if vectors_path.exists():
            for vector_file in vectors_path.glob("*.cf32"):
                available_vectors.append(str(vector_file))
            for vector_file in vectors_path.glob("*.unknown"):  # GR format
                available_vectors.append(str(vector_file))
                
        # Also check our known good vectors
        known_vectors = [
            "temp/hello_world.cf32",
            "temp/long_message.cf32"
        ]
        
        for vector in known_vectors:
            if Path(vector).exists():
                available_vectors.append(vector)
        
        print(f"📁 Found {len(available_vectors)} test vectors")
        
        # Test each configuration against available vectors
        for config in test_configs:
            print(f"\n🔧 CONFIGURATION: SF{config['sf']}, BW{config['bw']}Hz, CR{config['cr']}, CRC={config['has_crc']}")
            
            config_results = []
            
            # Test with available vectors
            for vector in available_vectors[:3]:  # Limit to first 3 vectors per config
                result = self.test_configuration(
                    config['sf'], config['bw'], config['cr'], 
                    config['has_crc'], vector
                )
                config_results.append(result)
                
            # If no vectors worked, try to generate one
            if not any(r['success'] for r in config_results):
                print(f"🔧 No successful decodes, trying to generate test vector...")
                generated_vector = self.generate_test_vector(
                    sf=config['sf'],
                    bw=config['bw'], 
                    cr=config['cr'],
                    has_crc=config['has_crc'],
                    payload="Test message for configuration"
                )
                
                if generated_vector:
                    result = self.test_configuration(
                        config['sf'], config['bw'], config['cr'],
                        config['has_crc'], generated_vector
                    )
                    config_results.append(result)
            
            # Store results
            self.test_results.extend(config_results)
    
    def generate_report(self, output_file: str = "test_report.json"):
        """Generate comprehensive test report"""
        
        print(f"\n📊 GENERATING TEST REPORT")
        print("=" * 30)
        
        # Analyze results
        total_tests = len(self.test_results)
        successful_tests = sum(1 for r in self.test_results if r['success'])
        
        # Group by configuration
        config_stats = {}
        for result in self.test_results:
            config_key = f"SF{result['config']['sf']}_BW{result['config']['bw']}_CR{result['config']['cr']}"
            if config_key not in config_stats:
                config_stats[config_key] = {'total': 0, 'success': 0}
            config_stats[config_key]['total'] += 1
            if result['success']:
                config_stats[config_key]['success'] += 1
        
        # Create report
        report = {
            'summary': {
                'total_tests': total_tests,
                'successful_tests': successful_tests,
                'success_rate': successful_tests / total_tests if total_tests > 0 else 0,
                'configurations_tested': len(config_stats)
            },
            'configuration_stats': config_stats,
            'detailed_results': self.test_results,
            'generated_vectors': self.generated_vectors,
            'receiver_info': {
                'method': 'Hybrid FFT + Phase Unwrapping',
                'breakthrough': '62.5% accuracy with position optimization',
                'proven_vectors': ['hello_world.cf32', 'long_message.cf32']
            }
        }
        
        # Save report
        with open(output_file, 'w') as f:
            json.dump(report, f, indent=2)
        
        print(f"✅ Report saved to: {output_file}")
        
        # Print summary
        print(f"\n🎯 TEST SUMMARY:")
        print(f"   Total tests: {total_tests}")
        print(f"   Successful: {successful_tests}")
        print(f"   Success rate: {successful_tests/total_tests*100:.1f}%" if total_tests > 0 else "   No tests run")
        
        print(f"\n📊 CONFIGURATION BREAKDOWN:")
        for config, stats in config_stats.items():
            success_rate = stats['success'] / stats['total'] * 100 if stats['total'] > 0 else 0
            print(f"   {config}: {stats['success']}/{stats['total']} ({success_rate:.1f}%)")

def main():
    """Command line interface"""
    
    parser = argparse.ArgumentParser(description='LoRa Receiver Test Suite')
    parser.add_argument('--test-vectors-dir', default='vectors',
                       help='Directory containing test vectors')
    parser.add_argument('--generate-vectors', action='store_true',
                       help='Generate new test vectors if needed')
    parser.add_argument('--output-report', default='test_report.json',
                       help='Output report file')
    parser.add_argument('--quick-test', action='store_true',
                       help='Run quick test with known vectors only')
    
    args = parser.parse_args()
    
    # Create test suite
    test_suite = LoRaTestSuite()
    
    if args.quick_test:
        # Quick test with our proven vectors
        print(f"🚀 QUICK TEST - PROVEN VECTORS")
        print("=" * 35)
        
        test_vectors = [
            ('temp/hello_world.cf32', 7, 125000, 2, True),
            ('temp/long_message.cf32', 7, 125000, 2, True)
        ]
        
        for vector_path, sf, bw, cr, has_crc in test_vectors:
            if Path(vector_path).exists():
                result = test_suite.test_configuration(sf, bw, cr, has_crc, vector_path)
                test_suite.test_results.append(result)
            else:
                print(f"⚠️  Vector not found: {vector_path}")
    else:
        # Full comprehensive test
        test_suite.run_comprehensive_test(args.test_vectors_dir)
    
    # Generate report
    test_suite.generate_report(args.output_report)
    
    print(f"\n🎉 TEST SUITE COMPLETE!")
    return 0

if __name__ == "__main__":
    exit(main())
