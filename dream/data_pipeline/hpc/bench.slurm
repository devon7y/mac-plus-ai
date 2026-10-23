#!/bin/bash
#SBATCH --account=def-jcaplan
#SBATCH --job-name=dream_bench
#SBATCH --time=00:30:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=6
#SBATCH --mem=64G
#SBATCH --gpus=h100:1
# FLUX 3 Image Klein throughput on one full GPU (no images kept): batch sizes in BENCH (default 8,16,32), steps in STEPS (default 8 and 16).
#   P=<project dir> sbatch --output=$P/logs/%x_%j.out --error=$P/logs/%x_%j.err --export=ALL,P=$P bench.slurm
set -uo pipefail
P=${P:?set P to the project dir}
source $P/scripts/hpc/mkenv.sh $SLURM_TMPDIR
C=$SLURM_TMPDIR/cache; mkdir -p $C/home
export HOME=$C/home XDG_CACHE_HOME=$C/xdg HF_HOME=$C/hf HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1 \
       TOKENIZERS_PARALLELISM=false PYTHONPATH=$P/flux3_ref/src
nvidia-smi --query-gpu=name,memory.total --format=csv,noheader
K=$SLURM_TMPDIR/flux-3-image-klein; mkdir -p $K
cp -r $P/models/flux-3-image-klein/{model.safetensors,config.json,manifest.json,ae.safetensors,text_encoder} $K/
cd $P/scripts
for st in ${STEPS:-8 16}; do
  python -u gen.py --backend klein --model $K --style caption_bbox --steps $st --bench ${BENCH:-8,16,32} \
    --out $P/data/bench_${SLURM_JOB_ID}_${st}steps
done
