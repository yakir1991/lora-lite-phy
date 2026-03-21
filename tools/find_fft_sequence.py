import sys

def find_sequence(filepath, target_sequence, tolerance=1):
    print(f"Searching in {filepath} for sequence {target_sequence} with tolerance +/- {tolerance}...")
    
    try:
        with open(filepath, 'r') as f:
            # Read all lines, strip whitespace, filter empty lines
            lines = [line.strip() for line in f if line.strip()]
            # Convert to integers
            values = [int(line) for line in lines]
    except Exception as e:
        print(f"Error reading file: {e}")
        return

    seq_len = len(target_sequence)
    if len(values) < seq_len:
        print("File too short.")
        return

    found = False
    for i in range(len(values) - seq_len + 1):
        match = True
        for j in range(seq_len):
            if abs(values[i+j] - target_sequence[j]) > tolerance:
                match = False
                break
        
        if match:
            found = True
            print(f"Found match at index {i}: {values[i:i+seq_len+5]}") # Print match + 5 next values
            # Don't break, find all matches

    if not found:
        print("No matching sequence found.")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 find_fft_sequence.py <filepath> <val1> <val2> ...")
        sys.exit(1)
        
    filepath = sys.argv[1]
    target_sequence = [int(x) for x in sys.argv[2:]]
    find_sequence(filepath, target_sequence)
