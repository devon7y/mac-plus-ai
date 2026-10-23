#!/bin/bash
#SBATCH --account=def-jcaplan
#SBATCH --job-name=dream_compare
#SBATCH --time=01:15:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=96G
#SBATCH --gpus=h100:1
# Generator comparison: the 20 pilot subjects x 4 seeds, one fixed template, on each model;
# throughput bench per model; CLIP scoring and first-pass filter per model; comparison sheets.
#   P=<project dir> sbatch --output=$P/logs/%x_%j.out --error=$P/logs/%x_%j.err compare.slurm
set -uo pipefail
P=${P:?set P to the project dir}
source $P/scripts/hpc/mkenv.sh $SLURM_TMPDIR
C=$SLURM_TMPDIR/cache; mkdir -p $C/home
export HOME=$C/home XDG_CACHE_HOME=$C/xdg HF_HOME=$C/hf HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1 \
       TOKENIZERS_PARALLELISM=false TORCHINDUCTOR_CACHE_DIR=$C/inductor TRITON_CACHE_DIR=$C/triton
nvidia-smi --query-gpu=name,memory.total --format=csv,noheader
D=$P/data/compare; mkdir -p $D
cd $P/scripts
T0=$(date +%s)
for pair in ${MODELS:-schnell:FLUX.1-schnell klein:FLUX.2-klein-4B}; do
  lab=${pair%%:*}; dir=${pair#*:}
  t=$(date +%s); M=$SLURM_TMPDIR/$dir
  cp -r $P/models/$dir $M
  echo "[$(date)] $lab: model copied in $(( $(date +%s) - t ))s"
  python -u gen.py --model $M --bench ${BENCH:-8,16} --subjects pilot_subjects.txt --n 4 --template 0 \
    --out $D/$lab/raw --batch 16
  echo "[$(date)] $lab: generation done in $(( $(date +%s) - t ))s"
  rm -rf $M
done
for pair in ${MODELS:-schnell:FLUX.1-schnell klein:FLUX.2-klein-4B}; do
  lab=${pair%%:*}
  python -u filter.py score --raw $D/$lab/raw --feat $D/$lab/feat --clip $P/models/CLIP-ViT-H-14-laion2B-s32B-b79K --force
  python -u filter.py select --feat $D/$lab/feat --out $D/$lab/split
done
python -u compare.py --models schnell=$D/schnell/raw klein=$D/klein/raw --out $D/sheets
echo "[$(date)] all done ($(( $(date +%s) - T0 ))s)"
