#!/usr/bin/env python3
import sys, pathlib, yaml

root = pathlib.Path(__file__).resolve().parent.parent
grc_dir = root / 'external' / 'gr_lora_sdr' / 'examples'

def strip_one(path: pathlib.Path) -> bool:
    try:
        doc = yaml.safe_load(path.read_text())
    except Exception as e:
        print(f"[strip-throttle] WARNING: failed to parse {path}: {e}")
        return False
    before_blocks = len(doc.get('blocks', []))
    def is_throttle(b):
        bid = b.get('id', '')
        return bid == 'blocks_throttle' or bid.startswith('blocks_throttle')
    doc['blocks'] = [b for b in doc.get('blocks', []) if not is_throttle(b)]
    removed_blocks = before_blocks - len(doc['blocks'])
    conns = doc.get('connections', [])
    new_conns = []
    removed_conns = 0
    for c in conns:
        s = ' '.join(map(str, c))
        if 'blocks_throttle' in s:
            removed_conns += 1
            continue
        new_conns.append(c)
    doc['connections'] = new_conns
    if removed_blocks or removed_conns:
        path.write_text(yaml.safe_dump(doc, sort_keys=False))
        print(f"[strip-throttle] {path.name}: removed {removed_blocks} blocks, {removed_conns} conns")
        return True
    return False

if __name__ == '__main__':
    changed = False
    for p in grc_dir.glob('*.grc'):
        changed |= strip_one(p)
    print('[strip-throttle] done')
