#!/usr/bin/env python3
import argparse, re
from pathlib import Path


def parse_lines(txt: str):
    # pattern: hdr_gr cwbytes (2blk samp0=0 off0=2 samp1=0 off1=-2 mode=raw): 00 74 ...
    pat = re.compile(
        r"hdr_gr cwbytes \(2blk samp0=([\-\d]+) off0=([\-\d]+) samp1=([\-\d]+) off1=([\-\d]+) mode=(raw|corr)\):\s+([0-9a-f]{2}(?:\s+[0-9a-f]{2}){9})",
        re.I,
    )
    for ln in txt.splitlines():
        m = pat.search(ln)
        if not m:
            continue
        samp0 = int(m.group(1))
        off0 = int(m.group(2))
        samp1 = int(m.group(3))
        off1 = int(m.group(4))
        mode = m.group(5)
        vals = [int(x, 16) for x in m.group(6).split()]
        yield {
            "samp0": samp0,
            "off0": off0,
            "samp1": samp1,
            "off1": off1,
            "mode": mode.lower(),
            "cw": vals,
        }


def main():
    ap = argparse.ArgumentParser(description="Parse hdr-single grid output and rank by diff vs GR")
    ap.add_argument("--in", default="logs/hdr_single_grid.out")
    ap.add_argument("--top", type=int, default=10)
    args = ap.parse_args()

    gr = [0x00, 0x74, 0xC5, 0x00, 0xC5, 0x1D, 0x12, 0x1B, 0x12, 0x00]
    txt = Path(getattr(args, "in")).read_text(errors="ignore")
    rows = list(parse_lines(txt))
    scored = []
    for r in rows:
        cw = r["cw"]
        full = sum(1 for a, b in zip(cw, gr) if a != b)
        last5 = sum(1 for a, b in zip(cw[5:], gr[5:]) if a != b)
        scored.append((full, last5, r))
    scored.sort(key=lambda x: (x[0], x[1]))
    out_lines = []
    for full, last5, r in scored[: args.top]:
        cwhex = " ".join(f"{x:02x}" for x in r['cw'])
        out_lines.append(
            f"diff={full} last5={last5} off1={r['off1']} samp1={r['samp1']} mode={r['mode']} cw={cwhex}"
        )
    print("\n".join(out_lines))


if __name__ == "__main__":
    raise SystemExit(main())


