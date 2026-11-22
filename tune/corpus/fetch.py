"""Download the Tune training sources into tune/data/raw/ (gitignored) and record checksums.

Sources and licences: see tune/README.md (checked 2026-10-23 before downloading).
  - O'Neill's Music of Ireland (1903, public domain), ABC by the O'Neill's Project (1997-2000):
    "You are welcome to download anything here and use it, as long as you give due credit to the
    transcribers" (+ GNU GPL v2 or later for redistribution).
  - Ryan's Mammoth Collection (1883, public domain), ABC by the Ryan/Cole team project (J. Chambers,
    R. Davies and others), published openly with no restrictions stated; we use only the notes
    (pitches, durations, bars), which are the public-domain melodies themselves.
  - Kerr's Merry Melodies 1-4 (Glasgow, c. 1875-1890, public domain), ABC by John Chambers
    (2016-2017), published openly with no restrictions stated; same basis as Ryan's.
Run: python3 -I tune/corpus/fetch.py
"""
import hashlib, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(HERE, "..", "data", "raw")
JC = "https://trillian.mit.edu/~jc/music/book/"
ONEILL_FILES = """0001-0050_np 0051-0100_jc 0101-0200_hn 0201-0300_jc 0301-0350_jc 0351-0400_cf
0401-0486_hn 0487-0500_jc 0501-0550_dw 0550-0625_jw 0626-0635_th 0626-0700_jc 0701-0720_lw
0721-0730_mp 0731-0731_hn 0732-0758_bs 0732-0758_mh 0759-0810_sf 0811-0899_dp 0900-0950_tk
0951-0981_dp 0981-1000_rt 1001-1031_dp 1031-1115_hn 1116-1135_mh 1116-1135_ml 1136-1175_sf
1176-1275_to 1276-1375_to 1376-1475_bs 1476-1555_jw 1556-1576_bh 1556-1624_mh 1577-1624_ml
1625-1700_nt 1701-1780_bs 1710-1750_lw 1781-1800_rt 1801-1850_bs""".split()

def get(url, dst):
    if not os.path.exists(dst):
        subprocess.run(["curl", "-sfL", "-o", dst, url], check=True)
    return hashlib.sha256(open(dst, "rb").read()).hexdigest()

def main():
    man = {}
    d = os.path.join(RAW, "oneills1850"); os.makedirs(d, exist_ok=True)
    for f in ONEILL_FILES:
        url = JC + "ONeills/_1850/F/" + f + ".abc"
        man[url] = get(url, os.path.join(d, f + ".abc"))
    d = os.path.join(RAW, "ryan_cole"); os.makedirs(d, exist_ok=True)
    url = JC + "ryan-cole/Ryan-Cole.abc"
    man[url] = get(url, os.path.join(d, "Ryan-Cole.abc"))
    d = os.path.join(RAW, "kerr"); os.makedirs(d, exist_ok=True)
    for i in range(1, 5):
        url = JC + "Kerr/MM%d-V1.abc" % i
        man[url] = get(url, os.path.join(d, "MM%d-V1.abc" % i))
    json.dump(man, open(os.path.join(RAW, "manifest.json"), "w"), indent=1)
    print(len(man), "files")

if __name__ == "__main__":
    main()
