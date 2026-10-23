#!/bin/bash
# Build the Dream training venv (torch, numpy, pillow, matplotlib) on node-local disk from the Alliance wheelhouse
# (imports from Lustre are slow). Usage: source mkenv.sh [dir]   (sets VENV, loads modules)
D=${1:-$SLURM_TMPDIR}/env
module load StdEnv/2023 gcc/12.3 cuda/12.6 python/3.11.5 >/dev/null 2>&1
if [ ! -x $D/bin/python ]; then
  virtualenv --no-download -q $D
  $D/bin/pip install -q --no-index --upgrade pip
  $D/bin/pip install -q --no-index torch triton numpy pillow matplotlib 2>&1 | grep -v "^\s*$" | tail -3
fi
VENV=$D
export PATH=$VENV/bin:$PATH VIRTUAL_ENV=$VENV
echo "[$(date)] venv ready: $($VENV/bin/python -c 'import torch; print("torch", torch.__version__, "cuda", torch.cuda.is_available())' 2>/dev/null)"
