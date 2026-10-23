#!/bin/bash
#SBATCH --account=def-jcaplan
#SBATCH --job-name=dtrain
#SBATCH --time=02:00:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=64G
#SBATCH --gpus=h100:1
# Generic Dream training job: builds the node-local venv, then runs the given command from the code
# folder (<P>/train/train). Usage (P = project dir on the cluster):
#   sbatch --output=$P/logs/%x_%j.out --error=$P/logs/%x_%j.err --job-name=dtrain_teacher \
#     --export=ALL,P=$P $P/train/train/hpc/job.slurm python -u train_teacher.py --data synth:500 --out $P/runs/x
set -uo pipefail
P=${P:?set P to the project dir}
source $P/train/train/hpc/mkenv.sh $SLURM_TMPDIR
C=$SLURM_TMPDIR/cache; mkdir -p $C
export HOME=$C XDG_CACHE_HOME=$C TORCHINDUCTOR_CACHE_DIR=$C/inductor TRITON_CACHE_DIR=$C/triton
nvidia-smi --query-gpu=name,memory.total --format=csv,noheader
# snapshot the code on node-local disk: later syncs to $P/train cannot change a running job (bash reads
# scripts incrementally, so editing one under a running job breaks it), and imports are faster
mkdir -p $SLURM_TMPDIR/code && cp -r $P/train/train $P/train/engine $P/train/data_pipeline $SLURM_TMPDIR/code/
cd $SLURM_TMPDIR/code/train
echo "[$(date)] $*"
T0=$(date +%s)
"$@"
rc=$?
echo "[$(date)] exit $rc after $(( $(date +%s) - T0 )) s"
exit $rc
