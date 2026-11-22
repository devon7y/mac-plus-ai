#!/bin/bash
# One tank type of a Tank Duel league (smaller jobs start sooner); the four jobs share the pool folder.
# for T in player rusher sniper careful; do sbatch --job-name=tanks_r4_$T --export=ALL,RUN=r4,TYPE=$T,W=14 league_type.slurm; done
#SBATCH --account=def-jcaplan
#SBATCH --output=/scratch/devon7y/macplus_tanks/logs/%x_%j.out
#SBATCH --error=/scratch/devon7y/macplus_tanks/logs/%x_%j.err
#SBATCH --time=05:00:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=16
#SBATCH --mem=24G
module load StdEnv/2023 python/3.12
virtualenv --no-download $SLURM_TMPDIR/env >/dev/null
source $SLURM_TMPDIR/env/bin/activate
pip install --no-index --quiet torch numpy
B=${BASE:-/scratch/devon7y/macplus_tanks}
cd $B/code/train
mkdir -p $B/runs/$RUN $B/pool/$RUN
python ppo.py --type $TYPE --out $B/runs/$RUN/$TYPE --pool $B/pool/$RUN --steps ${STEPS:-1e9} --n ${N:-8192} \
   --workers ${W:-14} --hidden ${HIDDEN:-32} --threads 2 --max_hours ${MAXH:-4.6} --seed $RANDOM \
   --lr 6e-4 --ent 0.005 --quant_at 0.7 --snap 20 --reload 10 $EXTRA
