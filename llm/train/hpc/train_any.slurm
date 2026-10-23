#!/bin/bash
#SBATCH --account=def-jcaplan
#SBATCH --time=01:30:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=64G
#SBATCH --gpus=h100:1
# Cluster-agnostic training job (Rorqual/Nibi hedge). Needs P (project dir), RUN, DATA (rel. to P),
# TOK (rel. to P); log paths are given on the sbatch line. Installs only torch + numpy (no vLLM).
#   sbatch --job-name=pixel_x --output=$P/logs/%x_%j.out --error=$P/logs/%x_%j.err \
#     --export=ALL,P=/scratch/devon7y/macplus_llm,RUN=pixel_x,DATA=data/v1/tok,TOK=data/v1/tokenizer.json,TRAIN_ARGS="--steps 12000" train_any.slurm
set -uo pipefail
E=${SLURM_TMPDIR:-/tmp/$USER-env}/env
module load StdEnv/2023 gcc/12.3 cuda/12.6 python/3.11.5 >/dev/null 2>&1
virtualenv --no-download -q $E
$E/bin/pip install -q --no-index --upgrade pip
$E/bin/pip install -q --no-index torch numpy 2>&1 | tail -3
C=${SLURM_TMPDIR:-/tmp/$USER-x}/cache; mkdir -p $C/home
export HOME=$C/home XDG_CACHE_HOME=$C/xdg TRITON_CACHE_DIR=$C/triton TORCHINDUCTOR_CACHE_DIR=$C/inductor
echo "[$(date)] venv ready: $($E/bin/python -c 'import torch; print(torch.__version__)')"
nvidia-smi --query-gpu=name,memory.total --format=csv,noheader
S=$P/scripts; O=$P/ckpt/$RUN; mkdir -p $O
DD=$P/$DATA
$E/bin/python -u $S/train.py --data $DD --out $O ${TRAIN_ARGS:-}
$E/bin/python -u $S/sample_chats.py --ckpt $O/best.pt --tok $P/$TOK --name $RUN --out $O \
  --val-text $(dirname $DD)/corpus/val.txt --val ${NVAL:-100}
echo TRAIN_JOB_DONE
