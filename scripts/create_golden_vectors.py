#!/usr/bin/env python3
"""
Create golden LoRa vectors using GNU Radio original lora_sdr with correct parameters.
This script ensures compatibility with our local decoder.
"""

import argparse
import sys
import os
import time
import numpy as np
from gnuradio import gr, blocks
import gnuradio.lora_sdr as lora_sdr
from gnuradio import channels
import pmt


class GoldenVectorGenerator(gr.top_block):
    def __init__(self, sf: int, cr_lora: int, payload_text: str | None,
                 payload_bytes: bytes | None,
                 bw_hz: int, samp_rate_hz: int,
                 sync_word: int = 0x12, preamble_len: int = 8,
                 out_iq_path: str = None, out_payload_path: str = None):
        gr.top_block.__init__(self, "golden_vector_generator", catch_exceptions=True)

        # Map LoRa CR (45,46,47,48) -> API (1..4)
        cr_map = {45:1, 46:2, 47:3, 48:4}
        cr_api = cr_map.get(cr_lora, 1)
        ldro = 2  # GNU Radio default

        # TX hierarchical block
        self.tx = lora_sdr.lora_sdr_lora_tx(
            bw=int(bw_hz), cr=int(cr_api), has_crc=True, impl_head=False,
            samp_rate=int(samp_rate_hz), sf=int(sf), ldro_mode=ldro,
            frame_zero_padd=int(20 * (2**sf) * samp_rate_hz / bw_hz), 
            sync_word=[int(sync_word)]
        )
        
        # RX hierarchical block  
        self.rx = lora_sdr.lora_sdr_lora_rx(
            bw=int(bw_hz), cr=int(cr_api), has_crc=True, impl_head=False, pay_len=255,
            samp_rate=int(samp_rate_hz), sf=int(sf), sync_word=[int(sync_word)],
            soft_decoding=True, ldro_mode=ldro, print_rx=[False, False]
        )

        # Channel model (no noise for golden vectors)
        self.chan = channels.channel_model(
            noise_voltage=0.0,
            frequency_offset=0.0,
            epsilon=1.0,
            taps=[1.0+0.0j],
            noise_seed=1,
            block_tags=True
        )
        
        # Set minimum output buffer
        self.chan.set_min_output_buffer(int((2**sf + 2) * samp_rate_hz / bw_hz))
        
        # Message source and sinks
        if payload_bytes is not None:
            u8 = pmt.init_u8vector(len(payload_bytes), payload_bytes)
            pdu = pmt.cons(pmt.PMT_NIL, u8)
            self.msg_src = blocks.message_strobe(pdu, 1000)
        else:
            assert payload_text is not None
            self.msg_src = blocks.message_strobe(pmt.intern(payload_text), 1000)
        self.msg_dbg = blocks.message_debug()
        
        # File sinks
        if out_iq_path:
            self.iq_sink = blocks.file_sink(gr.sizeof_gr_complex, out_iq_path, False)
            self.iq_sink.set_unbuffered(True)
        else:
            self.iq_sink = None
            
        # Do NOT write payload via RX; we'll write ground-truth TX payload after validation
        self.payload_sink = None

        # Connections
        self.connect(self.tx, self.chan)
        self.connect(self.chan, self.rx)

        # For clean deterministic IQ, tap TX directly (channel is noiseless anyway)
        if self.iq_sink:
            self.connect(self.tx, self.iq_sink)
            
        self.msg_connect(self.msg_src, 'strobe', self.tx, 'in')
        self.msg_connect(self.rx, 'out', self.msg_dbg, 'store')


def create_golden_vector(sf: int, cr: int, payload_text: str | None, payload_bytes: bytes | None,
                        bw: int = 125000, samp_rate: int = 125000,
                        sync_word: int = 0x12, preamble_len: int = 8,
                        out_iq: str = None, out_payload: str = None,
                        timeout: float = 10.0) -> bool:
    """Create a golden vector and validate it decodes correctly."""
    
    print(f"Creating golden vector: SF={sf}, CR={cr}, sync=0x{sync_word:02x}")
    if payload_bytes is not None:
        print(f"Payload: {len(payload_bytes)} bytes (binary)")
    else:
        print(f"Payload: '{payload_text}'")
    
    # Create the flowgraph
    tb = GoldenVectorGenerator(
        sf=sf, cr_lora=cr, payload_text=payload_text, payload_bytes=payload_bytes,
        bw_hz=bw, samp_rate_hz=samp_rate,
        sync_word=sync_word, preamble_len=preamble_len,
        out_iq_path=out_iq, out_payload_path=out_payload
    )
    
    try:
        # Start the flowgraph
        tb.start()
        
        # Wait for completion or timeout
        start_time = time.time()
        while time.time() - start_time < timeout:
            if tb.msg_dbg.num_messages() > 0:
                # Check if we got the expected payload
                msg = tb.msg_dbg.get_message(0)
                if pmt.is_symbol(msg) and payload_text is not None:
                    decoded_text = pmt.symbol_to_string(msg)
                    ok = (decoded_text == payload_text)
                    print(("✓ Success!" if ok else "✗ Mismatch!") + f" Decoded payload: '{decoded_text}'")
                    return ok
                elif pmt.is_pair(msg) and payload_bytes is not None:
                    v = pmt.cdr(msg)
                    if pmt.is_u8vector(v):
                        got = bytes(pmt.u8vector_elements(v))
                        ok = (got == payload_bytes)
                        print(("✓ Success!" if ok else "✗ Mismatch!") + f" Decoded {len(got)} bytes")
                        return ok
            time.sleep(0.1)
        
        print("✗ Timeout waiting for decoded payload")
        return False
        
    except Exception as e:
        print(f"✗ Error: {e}")
        return False
    finally:
        try:
            tb.stop()
            tb.wait()
        except:
            pass


def main():
    parser = argparse.ArgumentParser(description='Create golden LoRa vectors')
    parser.add_argument('--sf', type=int, required=True, help='Spreading factor (7-12)')
    parser.add_argument('--cr', type=int, required=True, help='Coding rate (45,46,47,48)')
    g = parser.add_mutually_exclusive_group(required=True)
    g.add_argument('--text', type=str, help='Payload text (ASCII)')
    g.add_argument('--zeros', type=int, help='Generate payload of N zero bytes')
    parser.add_argument('--bw', type=int, default=125000, help='Bandwidth (Hz)')
    parser.add_argument('--samp-rate', type=int, default=125000, help='Sampling rate (Hz)')
    parser.add_argument('--sync', type=lambda x: int(x, 0), default=0x12, help='Sync word (0x12 or 0x34)')
    parser.add_argument('--preamble-len', type=int, default=8, help='Preamble length')
    parser.add_argument('--out-iq', type=str, help='Output IQ file path')
    parser.add_argument('--out-payload', type=str, help='Output payload file path')
    parser.add_argument('--timeout', type=float, default=10.0, help='Timeout in seconds')
    
    args = parser.parse_args()
    
    # Validate arguments
    if args.sf < 7 or args.sf > 12:
        print("Error: SF must be between 7 and 12")
        return 1
        
    if args.cr not in [45, 46, 47, 48]:
        print("Error: CR must be 45, 46, 47, or 48")
        return 1
        
    if args.sync not in [0x12, 0x34]:
        print("Error: Sync word must be 0x12 or 0x34")
        return 1
    
    # Generate output filenames if not provided
    if not args.out_iq:
        args.out_iq = f"vectors/sf{args.sf}_cr{args.cr}_iq_sync{args.sync:02x}.bin"
    if not args.out_payload:
        args.out_payload = f"vectors/sf{args.sf}_cr{args.cr}_payload_sync{args.sync:02x}.bin"
    
    # Create output directory
    os.makedirs(os.path.dirname(args.out_iq), exist_ok=True)
    
    # Create the golden vector
    payload_bytes = (b"\x00" * args.zeros) if args.zeros is not None else None
    success = create_golden_vector(
        sf=args.sf, cr=args.cr, payload_text=args.text, payload_bytes=payload_bytes,
        bw=args.bw, samp_rate=args.samp_rate,
        sync_word=args.sync, preamble_len=args.preamble_len,
        out_iq=args.out_iq, out_payload=args.out_payload,
        timeout=args.timeout
    )
    
    if success:
        # Write intended TX payload directly to file
        if args.text is not None:
            try:
                with open(args.out_payload, 'wb') as f:
                    f.write(args.text.encode('ascii'))
            except Exception as e:
                print(f"Warning: failed to write payload file {args.out_payload}: {e}")
        elif args.zeros is not None:
            try:
                with open(args.out_payload, 'wb') as f:
                    f.write(b"\x00" * args.zeros)
            except Exception as e:
                print(f"Warning: failed to write payload file {args.out_payload}: {e}")

        print(f"✓ Golden vector created successfully!")
        print(f"  IQ file: {args.out_iq}")
        print(f"  Payload file: {args.out_payload}")
        return 0
    else:
        print("✗ Failed to create golden vector")
        return 1


if __name__ == '__main__':
    sys.exit(main())
