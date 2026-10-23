#!/bin/bash
# Build a vLLM + torch venv on node-local disk from the Alliance wheelhouse (CVMFS) in ~2 minutes.
# Importing vLLM from a venv on Lustre (/project, /scratch) took 15+ minutes on Fir compute nodes
# (2026-10-15), so every job builds its own. Usage: source mkenv.sh [dir]   (sets VENV, loads modules)
D=${1:-$SLURM_TMPDIR}/env
module load StdEnv/2023 gcc/12.3 cuda/12.6 python/3.11.5 opencv arrow >/dev/null 2>&1
if [ ! -x $D/bin/python ]; then
  virtualenv --no-download -q $D
  $D/bin/pip install -q --no-index --upgrade pip
  $D/bin/pip install -q --no-index vllm numpy 2>&1 | grep -v "^\s*$" | tail -3
fi
VENV=$D
export PATH=$VENV/bin:$PATH VIRTUAL_ENV=$VENV
echo "[$(date)] venv ready: $($VENV/bin/python -c 'import torch; print("torch", torch.__version__)' 2>/dev/null)"
