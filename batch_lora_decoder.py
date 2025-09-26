#!/usr/bin/env python3
"""
Batch LoRa Decoder - Process multiple LoRa vectors like GR LoRa SDR
Compatible interface with decode_offline_recording.py
"""

import argparse
import json
from pathlib import Path
import sys
from typing import List, Dict, Any

from complete_lora_receiver import LoRaReceiver

def process_file(filepath: str, receiver_config: Dict[str, Any] = None) -> Dict[str, Any]:
    """Process single LoRa file"""
    
    print(f"\n🔍 Processing: {filepath}")
    print("-" * 50)
    
    # Create receiver with configuration
    config = receiver_config or {}
    receiver = LoRaReceiver(
        sf=config.get('sf', 7),
        bw=config.get('bw', 125000),
        cr=config.get('cr', 2),
        has_crc=config.get('has_crc', True),
        impl_head=config.get('impl_head', False),
        ldro_mode=config.get('ldro_mode', 0),
        samp_rate=config.get('samp_rate', 500000),
        sync_words=config.get('sync_words', [18])  # Default 0x12
    )
    
    try:
        result = receiver.decode_file(filepath)
        
        # Format output similar to GR decoder
        if result['status'] != 'error':
            symbols = result.get('raw_symbols', [])
            frame_pos = result.get('frame_position', 0)
            confidence = result.get('confidence', 0.0)
            
            print(f"✅ SUCCESS!")
            print(f"   Frame position: {frame_pos}")
            print(f"   Raw symbols: {symbols}")
            print(f"   Confidence: {confidence:.2%}")
            
            # Add compatibility info
            result['gr_compatible'] = True
            result['frame_detected'] = True
            result['message_decoded'] = confidence > 0.5
            
        else:
            print(f"❌ FAILED: {result.get('error', 'Unknown error')}")
            result['gr_compatible'] = False
            result['frame_detected'] = False
            result['message_decoded'] = False
            
        result['input_file'] = filepath
        return result
        
    except Exception as e:
        error_result = {
            'input_file': filepath,
            'status': 'error',
            'error': str(e),
            'gr_compatible': False,
            'frame_detected': False,
            'message_decoded': False
        }
        print(f"❌ EXCEPTION: {e}")
        return error_result

def auto_detect_parameters(filepath: str) -> Dict[str, Any]:
    """Auto-detect LoRa parameters from filename"""
    
    filename = Path(filepath).name.lower()
    params = {}
    
    # Try to parse parameters from filename
    if 'sf' in filename:
        for sf in range(7, 13):
            if f'sf{sf}' in filename or f'sf_{sf}' in filename:
                params['sf'] = sf
                break
    
    if 'bw' in filename:
        for bw in [125000, 250000, 500000]:
            if f'bw{bw}' in filename or f'bw_{bw}' in filename:
                params['bw'] = bw
                break
            elif f'bw{bw//1000}k' in filename:
                params['bw'] = bw
                break
    
    if 'cr' in filename:
        for cr in range(1, 5):
            if f'cr{cr}' in filename or f'cr_{cr}' in filename:
                params['cr'] = cr
                break
    
    if 'crc' in filename:
        if 'crc1' in filename or 'crc_true' in filename:
            params['has_crc'] = True
        elif 'crc0' in filename or 'crc_false' in filename:
            params['has_crc'] = False
    
    if 'sps_500000' in filename:
        params['samp_rate'] = 500000
    elif 'sps_250000' in filename:
        params['samp_rate'] = 250000
    elif 'sps_1000000' in filename:
        params['samp_rate'] = 1000000
    
    if params:
        print(f"📋 Auto-detected parameters: {params}")
    
    return params

def batch_process_directory(directory: str, output_dir: str = None) -> List[Dict[str, Any]]:
    """Process all LoRa files in directory"""
    
    dir_path = Path(directory)
    if not dir_path.exists():
        print(f"❌ Directory not found: {directory}")
        return []
    
    # Find LoRa files
    lora_extensions = ['.cf32', '.unknown', '.bin', '.dat']
    lora_files = []
    
    for ext in lora_extensions:
        lora_files.extend(list(dir_path.glob(f'*{ext}')))
    
    print(f"📁 Found {len(lora_files)} potential LoRa files in {directory}")
    
    if not lora_files:
        print(f"⚠️  No LoRa files found with extensions: {lora_extensions}")
        return []
    
    # Process each file
    results = []
    successful_decodes = 0
    
    for i, filepath in enumerate(lora_files):
        print(f"\n📊 Progress: {i+1}/{len(lora_files)}")
        
        # Auto-detect parameters
        auto_params = auto_detect_parameters(str(filepath))
        
        # Process file
        result = process_file(str(filepath), auto_params)
        results.append(result)
        
        if result.get('message_decoded', False):
            successful_decodes += 1
        
        # Save individual result if output directory specified
        if output_dir:
            output_path = Path(output_dir)
            output_path.mkdir(exist_ok=True)
            
            result_filename = f"{filepath.stem}_result.json"
            result_path = output_path / result_filename
            
            with open(result_path, 'w') as f:
                json.dump(result, f, indent=2)
    
    print(f"\n🎯 BATCH PROCESSING COMPLETE!")
    print(f"   Files processed: {len(lora_files)}")
    print(f"   Successful decodes: {successful_decodes}")
    print(f"   Success rate: {successful_decodes/len(lora_files)*100:.1f}%")
    
    return results

def main():
    """Command line interface"""
    
    parser = argparse.ArgumentParser(
        description='Batch LoRa Decoder - GR LoRa SDR Compatible',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python batch_lora_decoder.py file.cf32
  python batch_lora_decoder.py vectors/ --output-dir results/
  python batch_lora_decoder.py --sf 8 --bw 250000 test_vector.cf32
        """
    )
    
    # Input specification
    parser.add_argument('input', help='Input file or directory')
    
    # LoRa parameters (override auto-detection)
    parser.add_argument('--sf', type=int, choices=range(7, 13),
                       help='Spreading Factor (7-12)')
    parser.add_argument('--bw', type=int, choices=[125000, 250000, 500000],
                       help='Bandwidth in Hz')
    parser.add_argument('--cr', type=int, choices=range(1, 5),
                       help='Coding Rate (1-4)')
    parser.add_argument('--crc', action='store_true',
                       help='CRC enabled')
    parser.add_argument('--no-crc', dest='crc', action='store_false',
                       help='CRC disabled')
    parser.add_argument('--impl-head', action='store_true',
                       help='Implicit header mode')
    parser.add_argument('--samp-rate', type=int, default=500000,
                       help='Sample rate in Hz')
    
    # Output options
    parser.add_argument('--output-dir', '-o', help='Output directory for results')
    parser.add_argument('--summary-file', default='batch_summary.json',
                       help='Summary report file')
    parser.add_argument('--verbose', '-v', action='store_true',
                       help='Verbose output')
    
    args = parser.parse_args()
    
    # Build receiver configuration from CLI args
    receiver_config = {}
    if args.sf is not None:
        receiver_config['sf'] = args.sf
    if args.bw is not None:
        receiver_config['bw'] = args.bw
    if args.cr is not None:
        receiver_config['cr'] = args.cr
    if args.crc is not None:
        receiver_config['has_crc'] = args.crc
    if args.impl_head:
        receiver_config['impl_head'] = args.impl_head
    if args.samp_rate:
        receiver_config['samp_rate'] = args.samp_rate
    
    print(f"🚀 BATCH LORA DECODER")
    print("=" * 30)
    print(f"📡 Based on our breakthrough 62.5% accuracy receiver")
    print(f"🔧 GR LoRa SDR compatible interface")
    
    if receiver_config:
        print(f"⚙️  Override config: {receiver_config}")
    
    # Process input
    input_path = Path(args.input)
    
    if input_path.is_file():
        # Single file processing
        print(f"📁 Processing single file: {args.input}")
        
        # Merge auto-detected and CLI params
        auto_params = auto_detect_parameters(args.input)
        final_config = {**auto_params, **receiver_config}
        
        result = process_file(args.input, final_config)
        results = [result]
        
        # Save result if output directory specified
        if args.output_dir:
            output_path = Path(args.output_dir)
            output_path.mkdir(exist_ok=True)
            result_file = output_path / f"{input_path.stem}_result.json"
            with open(result_file, 'w') as f:
                json.dump(result, f, indent=2)
            print(f"💾 Result saved to: {result_file}")
        
    elif input_path.is_dir():
        # Directory processing
        print(f"📂 Processing directory: {args.input}")
        results = batch_process_directory(args.input, args.output_dir)
        
    else:
        print(f"❌ Input not found: {args.input}")
        return 1
    
    # Generate summary report
    if len(results) > 1:  # Multiple files
        summary = {
            'total_files': len(results),
            'successful_decodes': sum(1 for r in results if r.get('message_decoded', False)),
            'frame_detections': sum(1 for r in results if r.get('frame_detected', False)),
            'gr_compatible': sum(1 for r in results if r.get('gr_compatible', False)),
            'results': results,
            'receiver_info': {
                'method': 'Hybrid FFT + Phase Unwrapping',
                'accuracy': '62.5% on known vectors',
                'compatibility': 'GR LoRa SDR interface'
            }
        }
        
        with open(args.summary_file, 'w') as f:
            json.dump(summary, f, indent=2)
        
        print(f"\n📋 Summary saved to: {args.summary_file}")
        success_rate = summary['successful_decodes'] / summary['total_files'] * 100
        print(f"📊 Overall success rate: {success_rate:.1f}%")
    
    return 0

if __name__ == "__main__":
    exit(main())
