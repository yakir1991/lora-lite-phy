#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VEC="$ROOT/vectors"
GRC_DIR="$ROOT/external/gr_lora_sdr"
GRC_FILE="$GRC_DIR/examples/tx_rx_simulation.grc"
PY_OUT="$GRC_DIR/tx_rx_simulation.py"

mkdir -p "$VEC"

if ! command -v grcc >/dev/null 2>&1; then
  echo "ERROR: grcc not found in PATH (activate conda env with gnuradio)." >&2
  exit 1
fi

PAIRS=(
  "7 45"
  "8 48"
  ${PAIRS_EXTRA:-}
)

patch_grc_and_run() {
  local SF="$1" CRLORA="$2"
  local CRAPI=1
  case "$CRLORA" in
    45) CRAPI=1;;
    46) CRAPI=2;;
    47) CRAPI=3;;
    48) CRAPI=4;;
    *) echo "Bad CR: $CRLORA"; exit 2;;
  esac

  local IQ="$VEC/sf${SF}_cr${CRLORA}_iq.bin"
  local PAY="$VEC/sf${SF}_cr${CRLORA}_payload.bin"

  # Create deterministic payload (16 bytes 0x01..0x10)
  python - <<PY
with open(r"$PAY", 'wb') as f:
    f.write(bytes(range(1,17)))
PY
  local PAYLEN
  PAYLEN=$(stat -c%s "$PAY")

  python - <<PY
import yaml, pathlib
grc_path = pathlib.Path(r"$GRC_FILE")
doc = yaml.safe_load(grc_path.read_text())

def set_var(name, val):
    for b in doc.get('blocks', []):
        if b.get('id') == 'variable' and b.get('name') == name:
            b.setdefault('parameters', {})['value'] = str(val)

# Configure variables; keep samp_rate == bw so N samples/symbol
set_var('samp_rate', 125000)
set_var('bw', 125000)
set_var('SNRdB', 100)
set_var('preamb_len', 8)
set_var('sf', int($SF))
set_var('cr', int($CRAPI))
set_var('pay_len', int($PAYLEN))

# Update source file path
for b in doc.get('blocks', []):
    if b.get('id') == 'blocks_file_source':
        p = b.setdefault('parameters', {})
        p['file'] = r"$PAY"
        p['repeat'] = False

# Remove throttle blocks if present
doc['blocks'] = [b for b in doc.get('blocks', []) if b.get('id') != 'blocks_throttle']

# Upsert sinks
def upsert_sink(name, ftype, path):
    blk = None
    for b in doc.get('blocks', []):
        if b.get('id')=='blocks_file_sink' and b.get('name')==name:
            blk=b; break
    if blk is None:
        blk = {
            'name': name,
            'id': 'blocks_file_sink',
            'parameters': {
                'affinity':'', 'alias':'', 'append': False, 'comment':'',
                'file': path, 'type': ftype, 'unbuffered': False, 'vlen':'1',
                'maxoutbuf':'0','minoutbuf':'0'
            },
            'states': {'bus_sink': False, 'bus_source': False, 'bus_structure': None,
                       'coordinate': [0,0], 'rotation': 0, 'state': 'enabled'}
        }
        doc.setdefault('blocks', []).append(blk)
    else:
        blk['parameters']['file'] = path
        blk['parameters']['type'] = ftype

upsert_sink('file_sink_tx_iq', 'complex', r"$IQ")
upsert_sink('file_sink_rx_payload', 'byte', r"/tmp/lora_rx_payload.bin")

def has_conn(src, s, dst, d):
    return [src,str(s),dst,str(d)] in doc.get('connections', [])

if not has_conn('lora_sdr_modulate_0','0','file_sink_tx_iq','0'):
    doc.setdefault('connections', []).append(['lora_sdr_modulate_0','0','file_sink_tx_iq','0'])
if not has_conn('lora_sdr_crc_verif_0','0','file_sink_rx_payload','0'):
    doc.setdefault('connections', []).append(['lora_sdr_crc_verif_0','0','file_sink_rx_payload','0'])

grc_path.write_text(yaml.safe_dump(doc, sort_keys=False))
print('patched', grc_path)
PY

  ( cd "$GRC_DIR" && grcc "$GRC_FILE" -o . )
  # Fix boolean arguments for file_sink in generated Python (GRC sometimes emits strings)
  python - <<PY || true
import re, pathlib
p = pathlib.Path(r"$PY_OUT")
s = p.read_text()
s = re.sub(r"blocks\.file_sink\(([^)]*),\s*'False'\)", r"blocks.file_sink(\1, False)", s)
s = s.replace("file_sink_tx_iq.set_unbuffered('False')", "file_sink_tx_iq.set_unbuffered(False)")
s = s.replace("file_sink_rx_payload.set_unbuffered('False')", "file_sink_rx_payload.set_unbuffered(False)")
p.write_text(s)
PY
  # Run the generated top block programmatically for a short duration
  PYTHONUNBUFFERED=1 timeout 30s python - <<PY
import sys, time
sys.path.insert(0, r"$GRC_DIR")
import tx_rx_simulation
tb = tx_rx_simulation.tx_rx_simulation()
tb.start()
time.sleep(2.0)
tb.stop(); tb.wait()
PY

  ls -lh "$IQ" "$PAY"
}

for p in "${PAIRS[@]}"; do
  SF="${p%% *}"; CR="${p##* }"
  echo "[*] Export via GRC: SF=$SF CR=$CR"
  patch_grc_and_run "$SF" "$CR"
done

echo "All vectors:"
ls -lh "$VEC"/*.bin
echo "Done."
