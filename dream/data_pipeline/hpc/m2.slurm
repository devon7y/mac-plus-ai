#!/bin/bash
#SBATCH --account=def-jcaplan
#SBATCH --job-name=dream_m2
#SBATCH --time=01:45:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=6
#SBATCH --mem=64G
#SBATCH --gpus=nvidia_h100_80gb_hbm3_3g.40gb:1
# Dream M2 data: one array task = every NTASKS-th subject of SUBJECTS (round-robin, as gen.py/filter.py),
# images i in [START, START+N) with FLUX 3 Image Klein (caption_bbox, 8 steps), then NSFW + CLIP scoring
# of this task's subjects. Resumable (finished images are skipped; rescoring is cheap).
#   P=<project dir> sbatch --array=0-29 --output=$P/logs/%x_%A_%a.out --error=$P/logs/%x_%A_%a.err \
#     --export=ALL,P=$P m2.slurm             (Nibi/Rorqual: add --gpus=h100:1, no MIG there)
set -uo pipefail
P=${P:?set P to the project dir}
NT=${NTASKS:-30}; TASK=${SLURM_ARRAY_TASK_ID:-0}; SUBJ=${SUBJECTS:-subjects.txt}
source $P/scripts/hpc/mkenv.sh $SLURM_TMPDIR
C=$SLURM_TMPDIR/cache; mkdir -p $C/home
export HOME=$C/home XDG_CACHE_HOME=$C/xdg HF_HOME=$C/hf HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1 \
       TOKENIZERS_PARALLELISM=false PYTHONPATH=$P/flux3_ref/src
nvidia-smi --query-gpu=name,memory.total --format=csv,noheader
D=$P/data/${RUN:-m2}; mkdir -p $D/raw $D/feat
cd $P/scripts
T0=$(date +%s)
K=$SLURM_TMPDIR/flux-3-image-klein; mkdir -p $K
cp -r $P/models/flux-3-image-klein/{model.safetensors,config.json,manifest.json,ae.safetensors,text_encoder} $K/
python -u gen.py --backend klein --model $K --style caption_bbox --steps ${STEPS:-8} --subjects $SUBJ \
  --task $TASK --ntasks $NT --start ${START:-0} --n ${N:-600} --out $D/raw --batch ${BATCH:-16}
echo "[$(date)] task $TASK generation done ($(( $(date +%s) - T0 ))s)"
python -u filter.py score --raw $D/raw --feat $D/feat --clip $P/models/CLIP-ViT-H-14-laion2B-s32B-b79K \
  --nsfw $P/models/nsfw-classifier --subjects $SUBJ --task $TASK --ntasks $NT --force
echo "[$(date)] task $TASK all done ($(( $(date +%s) - T0 ))s)"
