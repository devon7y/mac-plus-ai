#!/bin/bash
# Quantization-aware fine-tunes of earlier checkpoints (the Easy and Normal difficulty nets), all at once.
# SPECS="type:ckpt_step:tag:extra_steps ..." (ckpt_step as in the file name, e.g. 0031)
# sbatch --export=ALL,BASE=..,RUN=r3f[,LR=3e-4,W=20,MAXH=1.4],SPECS="rusher:0010:easy:12e6 rusher:0031:normal:12e6" qat_ft.slurm
#SBATCH --account=def-jcaplan
#SBATCH --output=/home/devon7y/scratch/devon7y/macplus_tanks/logs/%x_%j.out
#SBATCH --error=/home/devon7y/scratch/devon7y/macplus_tanks/logs/%x_%j.err
#SBATCH --time=01:40:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=96
#SBATCH --mem=128G
module load StdEnv/2023 python/3.12
virtualenv --no-download $SLURM_TMPDIR/env >/dev/null
source $SLURM_TMPDIR/env/bin/activate
pip install --no-index --quiet torch numpy
B=${BASE:-/home/devon7y/scratch/devon7y/macplus_tanks}
cd $B/code/train
mkdir -p $B/runs/ft
for spec in $SPECS; do
  IFS=: read T S TAG X <<< "$spec"
  TOTAL=$(python -c "print(int('$S') * 10**6 + int(float('$X')))")
  python ppo.py --type $T --resume $B/runs/$RUN/$T/ckpt_${S}M.pt --steps $TOTAL --quant_at 0 --lr ${LR:-1.5e-4} \
     --ent 0.003 --handicap 0 --pool $B/pool/ft_frozen --snap 1000000 --reload 1000000 --out $B/runs/ft/${T}_$TAG \
     --workers ${W:-14} --threads 1 --n 8192 --max_hours ${MAXH:-0.75} --seed $RANDOM > $B/runs/ft/${T}_$TAG.log 2>&1 &
done
wait
echo ALL DONE
