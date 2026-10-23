#!/bin/bash
#SBATCH --account=def-jcaplan
#SBATCH --job-name=dream_pilot
#SBATCH --time=02:00:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=96G
#SBATCH --gpus=h100:1
# Dream M0 data pilot with FLUX 3 Image Klein (distilled; STEPS, default 8): 20 subjects x N images, NSFW + CLIP
# scoring, first selection pass and contact sheets.
#   P=<project dir> STYLE=caption_bbox|caption|pup STEPS=8 N=70 \
#     sbatch --output=$P/logs/%x_%j.out --error=$P/logs/%x_%j.err --export=ALL,P=$P,STYLE=caption pilot.slurm
# SKIP_GEN=1 rescores existing images only.
set -uo pipefail
P=${P:?set P to the project dir}
STYLE=${STYLE:-caption_bbox}
source $P/scripts/hpc/mkenv.sh $SLURM_TMPDIR
C=$SLURM_TMPDIR/cache; mkdir -p $C/home
export HOME=$C/home XDG_CACHE_HOME=$C/xdg HF_HOME=$C/hf HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1 \
       TOKENIZERS_PARALLELISM=false TORCHINDUCTOR_CACHE_DIR=$C/inductor TRITON_CACHE_DIR=$C/triton \
       PYTHONPATH=$P/flux3_ref/src
nvidia-smi --query-gpu=name,memory.total --format=csv,noheader
D=$P/data/pilot; mkdir -p $D
cd $P/scripts
T0=$(date +%s)
if [ -z "${SKIP_GEN:-}" ]; then
  K=$SLURM_TMPDIR/flux-3-image-klein; mkdir -p $K
  cp -r $P/models/flux-3-image-klein/{model.safetensors,config.json,manifest.json,ae.safetensors,text_encoder} $K/
  CAPS=""
  if [ "$STYLE" = pup ]; then
    cp -r $P/models/flux-3-image-klein-pup $SLURM_TMPDIR/pup
    python -u upsample.py --pup $SLURM_TMPDIR/pup --subjects pilot_subjects.txt --n ${N:-70} --batch 64 \
      --out $D/captions_pup.jsonl
    rm -rf $SLURM_TMPDIR/pup; CAPS="--captions $D/captions_pup.jsonl"
  fi
  echo "[$(date)] models ready ($(( $(date +%s) - T0 ))s)"
  python -u gen.py --backend klein --model $K --style $STYLE $CAPS --steps ${STEPS:-8} --subjects pilot_subjects.txt \
    --n ${N:-70} --out $D/raw --batch ${BATCH:-16}
  echo "[$(date)] generation done ($(( $(date +%s) - T0 ))s since start)"
fi
python -u filter.py score --raw $D/raw --feat $D/feat --clip $P/models/CLIP-ViT-H-14-laion2B-s32B-b79K \
  --nsfw $P/models/nsfw-classifier ${SCORE_ARGS:-}
echo "[$(date)] scoring done ($(( $(date +%s) - T0 ))s since start)"
python -u filter.py select --feat $D/feat --out $D/split
python -u contact.py --split $D/split --feat $D/feat --out $D/sheets
echo "[$(date)] all done ($(( $(date +%s) - T0 ))s)"
