#!/bin/bash
#SBATCH --account=def-jcaplan
#SBATCH --job-name=pixel_train
#SBATCH --output=/home/devon7y/scratch/devon7y/macplus_llm/logs/%x_%j.out
#SBATCH --error=/home/devon7y/scratch/devon7y/macplus_llm/logs/%x_%j.err
#SBATCH --time=03:00:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=64G
#SBATCH --gpus=h100:1
#SBATCH --exclude=fc10512
# Train one Pixel model, then write 30 sample chats (+ val replies for the judge).
#   DATA=data/full/tok RUN=ternary_v1 TRAIN_ARGS="--steps 9000" sbatch train.slurm
#   DATA=data/full/tok RUN=fp_v1 TRAIN_ARGS="--fp --steps 9000" sbatch train.slurm
set -uo pipefail
P=/home/devon7y/scratch/devon7y/macplus_llm
source $P/scripts/mkenv.sh ${SLURM_TMPDIR:-/tmp/$USER-env}   # node-local venv from the wheelhouse
C=${SLURM_TMPDIR:-/tmp/$USER-x}/cache; mkdir -p $C/home
export HOME=$C/home XDG_CACHE_HOME=$C/xdg TRITON_CACHE_DIR=$C/triton TORCHINDUCTOR_CACHE_DIR=$C/inductor
nvidia-smi --query-gpu=name,memory.total --format=csv,noheader
S=$P/scripts; O=$P/ckpt/$RUN; mkdir -p $O
DD=$P/$DATA
$VENV/bin/python -u $S/train.py --data $DD --out $O ${TRAIN_ARGS:-}
$VENV/bin/python -u $S/sample_chats.py --ckpt $O/best.pt --tok $P/${TOK:-train/tokenizer.json} --name $RUN --out $O \
  --val-text $(dirname $DD)/corpus/val.txt --val ${NVAL:-300}
echo TRAIN_JOB_DONE
