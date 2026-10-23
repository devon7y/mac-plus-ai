#!/bin/bash
#SBATCH --account=def-jcaplan
#SBATCH --job-name=dream_m2_select
#SBATCH --time=01:00:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=16G
# Dream M2 final pass (CPU): thresholds + near-copy removal over all scored subjects, held-out split
# (8 per subject), paginated contact sheets, review sheets and stats.json.
#   P=<project dir> sbatch --output=$P/logs/%x_%j.out --error=$P/logs/%x_%j.err --export=ALL,P=$P m2_select.slurm
set -uo pipefail
P=${P:?set P to the project dir}
module load StdEnv/2023 python/3.11.5 scipy-stack >/dev/null 2>&1
D=$P/data/${RUN:-m2}
cd $P/scripts
python -u filter.py select --feat $D/feat --out $P/data/full --val-out $P/data/full_val --val-per-subject 8 ${TH:+--th $TH}
python -u contact.py --split $P/data/full --feat $D/feat --out $D/sheets --per-row 8 --per-line 2 --lines-per-page 20
echo "[$(date)] done"
