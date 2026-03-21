
import json
from generate_sim_captures import CONFIGS

def main():
    matrix = {
        "captures": [],
        "impairment_profiles": [
            {
                "label": "baseline",
                "description": "Reference condition without impairments"
            }
        ]
    }

    for cfg in CONFIGS:
        if not cfg["name"].startswith("sweep_"):
            continue
            
        entry = {
            "name": cfg["name"],
            "capture": f"gr_lora_sdr/data/generated/{cfg['output_name']}",
            "metadata": f"gr_lora_sdr/data/generated/{cfg['output_name'].replace('.cf32', '.json')}",
            "profiles": ["baseline"],
            "max_samples": int(cfg["sample_rate"] * 2.0) # 2 seconds should be enough for most
        }
        matrix["captures"].append(entry)

    with open("docs/receiver_vs_gnuradio_sweep_matrix.json", "w") as f:
        json.dump(matrix, f, indent=2)
    
    print("Wrote docs/receiver_vs_gnuradio_sweep_matrix.json")

if __name__ == "__main__":
    main()
