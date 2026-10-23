#!/bin/bash
#SBATCH --account=def-jcaplan
#SBATCH --job-name=pixel_prep
#SBATCH --output=/home/devon7y/scratch/devon7y/macplus_llm/logs/%x_%j.out
#SBATCH --error=/home/devon7y/scratch/devon7y/macplus_llm/logs/%x_%j.err
#SBATCH --time=03:00:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=32
#SBATCH --mem=128G
# Filter raw chats -> corpus text -> (tokenizer) -> token arrays.
#   RAW="data/raw/raw_*.jsonl" NAME=full TOK=train/tokenizer.json [TRAIN_TOK=1] sbatch prep.slurm
# Outputs: data/<NAME>/clean.jsonl, stats.json, corpus/{train,val}.txt, tok/{train,val}.bin ...
set -euo pipefail
P=/home/devon7y/scratch/devon7y/macplus_llm
source $P/scripts/mkenv.sh ${SLURM_TMPDIR:-/tmp/$USER-env}   # node-local venv from the wheelhouse
export HOME=${SLURM_TMPDIR:-/tmp}/home; mkdir -p $HOME
S=$P/scripts; D=$P/data/$NAME; mkdir -p $D
cd $P
$VENV/bin/python -u $S/filter.py --vocab $S/vocab.txt --out $D/clean.jsonl --stats $D/stats.json $RAW
$VENV/bin/python -u $S/build_corpus.py $D/clean.jsonl $D/corpus
TOKF=$P/$TOK
if [ "${TRAIN_TOK:-0}" = 1 ]; then
  mkdir -p $(dirname $TOKF)
  $VENV/bin/python -u $S/tokenizer.py $D/corpus/train.txt $TOKF ${TOK_LINES:-2000000}
fi
$VENV/bin/python -u $S/prepare.py $TOKF $D/corpus $D/tok $SLURM_CPUS_PER_TASK
echo PREP_DONE
