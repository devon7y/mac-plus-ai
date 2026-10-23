#!/bin/bash
# Build the project venv on a Fir login node (Alliance wheelhouse): vLLM + torch + tokenizers.
# Usage: bash setup_venv.sh   (from /home/devon7y/scratch/devon7y/macplus_llm)
set -e
P=/home/devon7y/scratch/devon7y/macplus_llm
module load StdEnv/2023 python/3.11.5 gcc/12.3 cuda/12.6 opencv arrow
cd $P
[ -d venv ] || virtualenv --no-download venv
source venv/bin/activate
pip install --no-index --upgrade pip
pip install --no-index vllm
pip install --no-index tokenizers transformers huggingface_hub numpy
python -c "import vllm, torch, tokenizers; print('vllm', vllm.__version__, 'torch', torch.__version__)"
echo SETUP_DONE
