#!/bin/bash
# Build the Dream data venv on node-local disk from the Alliance wheelhouse (imports from Lustre are
# slow). Usage: source mkenv.sh [dir]   (sets VENV, loads modules)
D=${1:-$SLURM_TMPDIR}/env
module load StdEnv/2023 gcc/12.3 cuda/12.6 python/3.11.5 >/dev/null 2>&1
if [ ! -x $D/bin/python ]; then
  virtualenv --no-download -q $D
  $D/bin/pip install -q --no-index --upgrade pip
  $D/bin/pip install -q --no-index torch diffusers transformers accelerate sentencepiece protobuf \
    safetensors open_clip_torch pillow numpy scipy 2>&1 | grep -v "^\s*$" | tail -3
fi
VENV=$D
export PATH=$VENV/bin:$PATH VIRTUAL_ENV=$VENV
echo "[$(date)] venv ready: $($VENV/bin/python -c 'import torch, diffusers, transformers, open_clip; print("torch", torch.__version__, "diffusers", diffusers.__version__, "transformers", transformers.__version__, "open_clip", open_clip.__version__)' 2>&1 | tail -1)"
