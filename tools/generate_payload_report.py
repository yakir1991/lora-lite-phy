
import json
import sys
from pathlib import Path

def main():
    if len(sys.argv) < 3:
        print("Usage: python3 generate_payload_report.py <input_json> <output_md>")
        sys.exit(1)

    input_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2])

    if not input_path.exists():
        print(f"Error: {input_path} not found")
        sys.exit(1)

    data = json.loads(input_path.read_text())

    lines = []
    lines.append("# Payload Comparison Report")
    lines.append("")
    lines.append("| Capture | Profile | Match | GNU Radio Payload (Hex) | Standalone Payload (Hex) |")
    lines.append("|---|---|---|---|---|")

    match_count = 0
    total_count = 0

    for item in data:
        capture = item.get("capture", "unknown")
        profile = item.get("profile", "unknown")
        payload = item.get("payload", {})
        expected = payload.get("expected_hex", "N/A")
        standalone = payload.get("standalone_hex", "N/A")
        match = item.get("payload_match", False)

        if match:
            match_str = "**Yes**"
            match_count += 1
        else:
            match_str = "No"

        # Truncate long payloads for display if needed, but user asked for full check
        # Let's keep them full for now, or maybe truncate if > 64 chars?
        # User asked for "full compatibility check", so seeing the full payload is good.
        
        lines.append(f"| {capture} | {profile} | {match_str} | `{expected}` | `{standalone}` |")
        total_count += 1

    lines.append("")
    lines.append(f"**Summary:** {match_count}/{total_count} payloads matched.")

    output_path.write_text("\n".join(lines))
    print(f"Wrote report to {output_path}")

if __name__ == "__main__":
    main()
