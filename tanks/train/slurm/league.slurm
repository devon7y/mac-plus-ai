#!/bin/bash
# Tank Duel league: the four tank types train at once on one node, sharing the pool folder.
# sbatch --job-name=tanks_league --export=ALL,RUN=r1,STEPS=120e6[,HIDDEN=32,QAT=0.6,EXTRA=..] league.slurm
#SBATCH --account=def-jcaplan
#SBATCH --output=/scratch/devon7y/macplus_tanks/logs/%x_%j.out
#SBATCH --error=/scratch/devon7y/macplus_tanks/logs/%x_%j.err
#SBATCH --time=04:00:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=32
#SBATCH --mem=48G
module load StdEnv/2023 python/3.12
virtualenv --no-download $SLURM_TMPDIR/env >/dev/null
source $SLURM_TMPDIR/env/bin/activate
pip install --no-index --quiet torch numpy
B=/scratch/devon7y/macplus_tanks
cd $B/code/train
mkdir -p $B/runs/$RUN $B/pool/$RUN
for T in player rusher sniper careful; do
  python ppo.py --type $T --out $B/runs/$RUN/$T --pool $B/pool/$RUN --steps ${STEPS:-120e6} \
     --hidden ${HIDDEN:-32} --quant_at ${QAT:-0.6} --threads 8 --max_hours ${MAXH:-3.6} --seed $RANDOM $EXTRA \
     > $B/runs/$RUN/$T.log 2>&1 &
done
wait
echo ALL DONE
