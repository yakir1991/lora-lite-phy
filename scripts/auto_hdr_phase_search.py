#!/usr/bin/env python3
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path


def run_with_heartbeat(cmd: str, env: dict | None, timeout_per_anchor: int, beat_every: float = 2.0):
    """Run a command with periodic heartbeat prints while waiting.
    Returns (rc, out_str, err_str).
    """
    p = subprocess.Popen(
        cmd,
        shell=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
    )
    start = time.time()
    last = 0.0
    out = b""; err = b""
    while True:
        try:
            o, e = p.communicate(timeout=1)
            out += o or b""; err += e or b""
            break
        except subprocess.TimeoutExpired:
            # heartbeat
            elapsed = time.time() - start
            if elapsed - last >= beat_every:
                print(f"[auto]  … still running t={int(elapsed)}s (anchor timeout={timeout_per_anchor}s)")
                sys.stdout.flush()
                last = elapsed
            if elapsed >= timeout_per_anchor:
                p.kill()
                try:
                    o, e = p.communicate(timeout=2)
                    out += o or b""; err += e or b""
                except Exception:
                    pass
                return -9, out.decode(errors='ignore'), err.decode(errors='ignore')
    return p.returncode, out.decode(errors='ignore'), err.decode(errors='ignore')


def main():
    ap = argparse.ArgumentParser(description="Automatic header phase search with guided Block-1 alignment")
    ap.add_argument("--exe", default=str(Path("build/lora_decode").resolve()), help="Path to lora_decode executable")
    ap.add_argument("--vec", default=str(Path("vectors/bw_125k_sf_7_cr_1_ldro_false_crc_true_implheader_false.unknown").resolve()), help="Input IQ vector")
    ap.add_argument("--sf", type=int, default=7)
    ap.add_argument("--cr", type=int, default=45)
    ap.add_argument("--sync", default="0x12")
    ap.add_argument("--min-pre", type=int, default=8)
    ap.add_argument("--out", default=str(Path("logs/auto_hdr_phase.json").resolve()))
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--anchor-timeout", type=int, default=25, help="Max seconds per anchor before moving on")
    # Expanded grid controls
    ap.add_argument("--sym-min", type=int, default=-2, help="Minimum symbol-offset around base header anchor")
    ap.add_argument("--sym-max", type=int, default=2, help="Maximum symbol-offset around base header anchor")
    ap.add_argument("--samp-min", type=int, default=-80, help="Minimum sample-offset around base header anchor")
    ap.add_argument("--samp-max", type=int, default=80, help="Maximum sample-offset around base header anchor")
    ap.add_argument("--samp-step", type=int, default=8, help="Step for sample-offset grid")
    ap.add_argument("--cfo-modes", default="2", help="Comma-separated LORA_HDR_BLOCK_CFO modes to try per anchor (e.g., 2,1,0)")
    ap.add_argument("--guided-log", action="store_true", help="Enable LORA_DEBUG=1 to collect [hdr-guided] metrics")
    args = ap.parse_args()

    exe = args.exe
    vec = args.vec
    logdir = Path("logs"); logdir.mkdir(parents=True, exist_ok=True)

    # Search spaces (configurable)
    sym_offs = list(range(args.sym_min, args.sym_max + 1))
    if args.samp_step <= 0:
        args.samp_step = 1
    samp_offs = list(range(args.samp_min, args.samp_max + 1, args.samp_step))
    cfo_modes = []
    for tok in str(args.cfo_modes).split(','):
        tok = tok.strip()
        if not tok:
            continue
        if tok in ("0", "1", "2"):
            cfo_modes.append(tok)
    if not cfo_modes:
        cfo_modes = ["2"]

    start = time.time()
    best = {"match": -1, "rot": None, "so": None, "sa": None}
    tried = 0
    total = len(sym_offs) * len(samp_offs) * len(cfo_modes)
    records = []

    print(f"[auto] starting header phase search ({total} anchors), timeout={args.timeout}s")
    sys.stdout.flush()

    for so in sym_offs:
        for sa in samp_offs:
            for cfo_mode in cfo_modes:
                tried += 1
                env = os.environ.copy()
                env.update({
                    "LORA_HDR_BASE_SYM_OFF": str(so),
                    "LORA_HDR_BASE_SAMP_OFF": str(sa),
                    "LORA_HDR_BLOCK_CFO": cfo_mode,
                    "LORA_HDR_MICRO": "1",
                    "LORA_HDR_MICRO_WIDE": "1",
                    "LORA_HDR_SLOPE": "1",
                    "LORA_HDR_SLOPE_WIDE": "1",
                    "LORA_HDR_EPS_WIDE": "1",
                    "LORA_HDR_GNU_B1": "0,0,0,22,12,1,5,14",
                    "LORA_HDR_LOG_GNU": "1",
                })
                if args.guided_log:
                    env["LORA_DEBUG"] = "1"
                cmd = (
                    f"{exe} --in {vec} --sf {args.sf} --cr {args.cr} --sync {args.sync} "
                    f"--min-preamble {args.min_pre} --json"
                )
                print(f"[auto] start {tried}/{total} so={so} sa={sa} cfo={cfo_mode}")
                sys.stdout.flush()
                rc, out, err = run_with_heartbeat(cmd, env, timeout_per_anchor=args.anchor_timeout)
                # Progress print
                elapsed = time.time() - start
                rate = tried / max(elapsed, 1e-6)
                eta = (total - tried) / max(rate, 1e-6)
                # Parse guided line if exists
                match = -1; rot = None
                for line in err.splitlines():
                    if "[hdr-guided]" in line and "match=" in line:
                        try:
                            part = line.split("match=")[-1]
                            k = int(part.split(")")[0])
                            match = k
                            if "rot=" in line:
                                rpart = line.split("rot=")[-1]
                                rot = int(rpart.split(",")[0])
                        except Exception:
                            pass
                if match > best["match"]:
                    best = {"match": match, "rot": rot, "so": so, "sa": sa, "cfo": cfo_mode}
                # Try to parse JSON to detect success
                ok = False; reason = None
                try:
                    j = json.loads(out) if out.strip() else {}
                    ok = bool(j.get("success", False))
                    reason = j.get("reason")
                except Exception:
                    ok = False
                    reason = None
                records.append({"so": so, "sa": sa, "cfo": cfo_mode, "ok": ok, "match": match, "rot": rot, "reason": reason})
                print(f"[auto] {tried}/{total} so={so} sa={sa} cfo={cfo_mode} match={match} rot={rot} rate={rate:.1f}/s ETA={eta:.0f}s ok={ok} {reason or ''}")
                sys.stdout.flush()
                if ok:
                    with open(args.out, "w") as f:
                        json.dump({"ok": True, "best": best, "anchor": {"sym_off": so, "samp_off": sa, "cfo": cfo_mode}, "json": out, "records": records}, f, indent=2)
                    print("[auto] header decode succeeded, stopping.")
                    return 0
                if elapsed > args.timeout:
                    print("[auto] timeout reached, stopping.")
                    with open(args.out, "w") as f:
                        json.dump({"ok": False, "best": best, "reason": "timeout", "records": records}, f, indent=2)
                    return 2

    with open(args.out, "w") as f:
        json.dump({"ok": False, "best": best, "reason": "exhausted", "records": records}, f, indent=2)
    print("[auto] exhausted all anchors without CRC pass.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
