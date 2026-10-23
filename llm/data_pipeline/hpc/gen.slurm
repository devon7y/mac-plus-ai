#!/bin/bash
#SBATCH --account=def-jcaplan
#SBATCH --job-name=pixel_gen
#SBATCH --output=/home/devon7y/scratch/devon7y/macplus_llm/logs/%x_%A_%a.out
#SBATCH --error=/home/devon7y/scratch/devon7y/macplus_llm/logs/%x_%A_%a.err
#SBATCH --time=03:00:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=64G
#SBATCH --gpus=h100:1
#SBATCH --exclude=fc10512
# Pixel chat generation with Qwen2.5-7B-Instruct (Apache-2.0) under vLLM 0.30 (Fir).
#   pilot:  REQUESTS=10000 SEED0=0 OUTDIR=data/pilot sbatch --array=0 gen.slurm
#   full:   REQUESTS=40000 SEED0=100 OUTDIR=data/raw sbatch --array=0-15 gen.slurm
# Each array task writes $OUTDIR/raw_<seed>.jsonl (resumable: resubmitting skips finished requests).
set -uo pipefail
P=/home/devon7y/scratch/devon7y/macplus_llm
if [ "${VENV_MODE:-local}" = project ]; then   # shared vLLM 0.30 venv (slow Lustre import, 15+ min)
  VENV=/project/def-jcaplan/devon7y/venv_vllm_030
  module load StdEnv/2023 python/3.12 >/dev/null 2>&1
  export PATH=$VENV/bin:$PATH VIRTUAL_ENV=$VENV CUDA_HOME=$VENV/lib/python3.12/site-packages/nvidia/cu13
  export PATH=$CUDA_HOME/bin:$PATH
else
  source $P/scripts/mkenv.sh ${SLURM_TMPDIR:-/tmp/$USER-env}   # node-local venv from the wheelhouse
fi
C=${SLURM_TMPDIR:-/tmp/$USER-${SLURM_JOB_ID:-x}}/cache; mkdir -p $C/home
export HOME=$C/home XDG_CACHE_HOME=$C/xdg FLASHINFER_WORKSPACE_BASE=$C HF_HUB_OFFLINE=1 \
       TRANSFORMERS_OFFLINE=1 TOKENIZERS_PARALLELISM=false VLLM_WORKER_MULTIPROC_METHOD=spawn \
       VLLM_CACHE_ROOT=$C/vllm TRITON_CACHE_DIR=$C/triton TORCHINDUCTOR_CACHE_DIR=$C/inductor \
       VLLM_USE_FLASHINFER_SAMPLER=0
nvidia-smi --query-gpu=name,memory.total --format=csv,noheader

TASK=${SLURM_ARRAY_TASK_ID:-0}
SEED=$(( ${SEED0:-0} + TASK ))
OUTDIR=$P/${OUTDIR:-data/pilot}; mkdir -p $OUTDIR
M=$SLURM_TMPDIR/Qwen2.5-7B-Instruct
cp -r $P/models/Qwen2.5-7B-Instruct $M
echo "[$(date)] model copied; seed $SEED"
cd $P/scripts
$VENV/bin/python -u gen.py --model $M --vocab $P/scripts/vocab.txt --out $OUTDIR/raw_$(printf %04d $SEED).jsonl \
  --requests ${REQUESTS:-10000} --seed $SEED --batch ${BATCH:-2000} ${GEN_ARGS:-}
echo "[$(date)] exit $?"
