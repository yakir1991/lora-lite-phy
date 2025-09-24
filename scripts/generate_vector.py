import numpy as np
import struct

# Function to generate a LoRa vector file
def generate_lora_vector(file_path, payload, sf, cr, os, crc, header):
    # Metadata
    metadata = f"sf_{sf}_cr_{cr}_os{os}_crc_{'true' if crc else 'false'}_header_{'true' if header else 'false'}"

    # Generate dummy IQ samples (replace with actual LoRa modulation if needed)
    num_samples = 1000  # Adjust as needed
    iq_samples = np.random.randn(num_samples, 2).astype(np.float32)  # Random IQ samples

    # Write to file
    with open(file_path, "wb") as f:
        # Write metadata as a comment
        f.write(f"# {metadata}\n".encode("utf-8"))

        # Write payload
        f.write(f"# Payload: {payload}\n".encode("utf-8"))

        # Write IQ samples
        for sample in iq_samples:
            f.write(struct.pack("ff", *sample))

# Parameters
file_path = "../vectors/sps_125k_sf_7_cr_1_ldro_false_crc_true_implheader_true_hello_yakir.unknown"
payload = "HELLO YAKIR"
sf = 7
cr = 1
os = 1
crc = True
header = True

# Generate the vector
generate_lora_vector(file_path, payload, sf, cr, os, crc, header)
print(f"Vector file generated: {file_path}")