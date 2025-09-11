#!/usr/bin/env python3
from pathlib import Path

text = Path('src/rx/frame.cpp').read_text().splitlines()
start = None
end = None
for i, line in enumerate(text, start=1):
    if 'std::optional<LocalHeader> decode_header_with_preamble_cfo_sto_os' in line:
        start = i
    if 'std::pair<std::vector<uint8_t>, bool> decode_payload_no_crc_with_preamble_cfo_sto_os' in line:
        end = i
        break
print('start', start, 'end', end)
bal = 0
for i in range(start, end):
    for ch in text[i-1]:
        if ch == '{':
            bal += 1
        elif ch == '}':
            bal -= 1
print('section brace balance', bal)

