#!/usr/bin/env python3
"""
Test original receiver without known position heuristic
"""

import subprocess
import tempfile
import shutil
import os

def test_original_without_heuristic():
    """Test original receiver with known position heuristic disabled"""
    
    # Create a temporary copy of complete_lora_receiver.py with heuristic disabled
    with open('complete_lora_receiver.py', 'r') as f:
        content = f.read()
    
    # Disable the known position heuristic
    modified_content = content.replace(
        'if "hello_world" in str(samples).lower() or len(samples) == 78080:',
        'if False:  # Disabled for testing'
    )
    
    # Write to temporary file
    with open('complete_lora_receiver_temp.py', 'w') as f:
        f.write(modified_content)
    
    try:
        # Run the modified receiver
        cmd = ["python3", "complete_lora_receiver_temp.py", 
               "vectors/sps_500k_bw_125k_sf_7_cr_2_ldro_false_crc_true_implheader_false_hello_stupid_world.unknown"]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        
        print("📊 ORIGINAL RECEIVER (NO KNOWN POSITION):")
        print("=" * 50)
        print(result.stdout)
        if result.stderr:
            print("STDERR:")
            print(result.stderr)
        
    finally:
        # Clean up temporary file
        if os.path.exists('complete_lora_receiver_temp.py'):
            os.remove('complete_lora_receiver_temp.py')

if __name__ == "__main__":
    test_original_without_heuristic()
