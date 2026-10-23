#!/bin/bash
#SBATCH --account=def-jcaplan
#SBATCH --job-name=dream_klein_test
#SBATCH --time=01:15:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=96G
#SBATCH --gpus=h100:1
# FLUX 3 Image Klein caption test: 20 pilot subjects x 2 seeds, same seeds for every variant:
#   caption       hand-written full captions (prompts.CAPTIONS), 16 steps
#   pup           short request -> BFL prompt upsampler (flux-3-image-klein-pup) -> caption, 16 steps
#   caption_bbox  hand-written caption + centred layout box, 16 steps
#   caption_8     hand-written captions, 8 steps (speed/quality trade-off)
# plus a batch-size throughput bench, NSFW + CLIP scoring, first-pass filter and comparison sheets.
#   P=<project dir> sbatch --output=$P/logs/%x_%j.out --error=$P/logs/%x_%j.err --export=ALL,P=$P klein_test.slurm
set -uo pipefail
P=${P:?set P to the project dir}
source $P/scripts/hpc/mkenv.sh $SLURM_TMPDIR
C=$SLURM_TMPDIR/cache; mkdir -p $C/home
export HOME=$C/home XDG_CACHE_HOME=$C/xdg HF_HOME=$C/hf HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1 \
       TOKENIZERS_PARALLELISM=false TORCHINDUCTOR_CACHE_DIR=$C/inductor TRITON_CACHE_DIR=$C/triton \
       PYTHONPATH=$P/flux3_ref/src
nvidia-smi --query-gpu=name,memory.total --format=csv,noheader
D=$P/data/klein_test${TAG:-}; mkdir -p $D
cd $P/scripts
T0=$(date +%s)
K=$SLURM_TMPDIR/flux-3-image-klein; mkdir -p $K
cp -r $P/models/flux-3-image-klein/{model.safetensors,config.json,manifest.json,ae.safetensors,text_encoder} $K/
cp -r $P/models/flux-3-image-klein-pup $SLURM_TMPDIR/pup
echo "[$(date)] models copied in $(( $(date +%s) - T0 ))s"

python -u upsample.py --pup $SLURM_TMPDIR/pup --subjects pilot_subjects.txt --n 2 --out $D/captions_pup.jsonl
rm -rf $SLURM_TMPDIR/pup
echo "[$(date)] upsampling done ($(( $(date +%s) - T0 ))s)"

G="python -u gen.py --backend klein --model $K --subjects pilot_subjects.txt --n 2 --batch 20"
$G --style caption --out $D/caption/raw --bench ${BENCH:-4,8,16,32}
$G --style pup --captions $D/captions_pup.jsonl --out $D/pup/raw
$G --style caption_bbox --out $D/caption_bbox/raw
$G --style caption --steps 8 --out $D/caption_8/raw
echo "[$(date)] generation done ($(( $(date +%s) - T0 ))s)"

for v in caption pup caption_bbox caption_8; do
  python -u filter.py score --raw $D/$v/raw --feat $D/$v/feat --clip $P/models/CLIP-ViT-H-14-laion2B-s32B-b79K \
    --nsfw $P/models/nsfw-classifier --force
  python -u filter.py select --feat $D/$v/feat --out $D/$v/split
done
python -u compare.py --models caption=$D/caption/raw pup=$D/pup/raw caption_bbox=$D/caption_bbox/raw \
  caption_8steps=$D/caption_8/raw --out $D/sheets
echo "[$(date)] all done ($(( $(date +%s) - T0 ))s)"
