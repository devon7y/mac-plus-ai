#!/bin/bash
# Tank Duel league on a whole node: the four tank types at once, each with W worker processes
# running shards of the games (the numpy twin is single-threaded), sharing the pool folder.
# sbatch --job-name=tanks_r3 --export=ALL,RUN=r3,W=14,N=8192,STEPS=1e9,MAXH=5.5[,EXTRA=..] league_vec.slurm
#SBATCH --account=def-jcaplan
#SBATCH --output=/scratch/devon7y/macplus_tanks/logs/%x_%j.out
#SBATCH --error=/scratch/devon7y/macplus_tanks/logs/%x_%j.err
#SBATCH --time=06:00:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=64
#SBATCH --mem=96G
module load StdEnv/2023 python/3.12
virtualenv --no-download $SLURM_TMPDIR/env >/dev/null
source $SLURM_TMPDIR/env/bin/activate
pip install --no-index --quiet torch numpy
B=${BASE:-/scratch/devon7y/macplus_tanks}
cd $B/code/train
mkdir -p $B/runs/$RUN $B/pool/$RUN
for T in player rusher sniper careful; do
  python ppo.py --type $T --out $B/runs/$RUN/$T --pool $B/pool/$RUN --steps ${STEPS:-1e9} --n ${N:-8192} \
     --workers ${W:-14} --hidden ${HIDDEN:-32} --threads 2 --max_hours ${MAXH:-5.5} --seed $RANDOM $EXTRA \
     > $B/runs/$RUN/$T.log 2>&1 &
done
wait
echo ALL DONE
