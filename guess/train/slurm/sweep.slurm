#!/bin/bash
# Train several architectures one after another (float or QAT), each with its own run folder.
# sbatch --job-name=guess_sweepA --export=ALL,ARCHS="name1=spec1;name2=spec2",STEPS=30000[,CATS=..,EXTRA=..] sweep.slurm
#SBATCH --account=def-jcaplan
#SBATCH --output=/home/devon7y/macplus_guess/logs/%x_%j.out
#SBATCH --error=/home/devon7y/macplus_guess/logs/%x_%j.err
#SBATCH --time=02:30:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=64G
#SBATCH --gpus=h100:1
module load StdEnv/2023 python/3.12
source /scratch/devon7y/macplus_paint/venv/bin/activate
cd /home/devon7y/macplus_guess/code
IFS=';' read -ra LIST <<< "$ARCHS"
for item in "${LIST[@]}"; do
  name=${item%%=*}; spec=${item#*=}
  echo "=== $name $spec"
  python train.py --prep /scratch/devon7y/macplus_paint/data/prep --arch "$spec" --out ../runs/$name \
     --steps ${STEPS:-30000} ${CATS:+--cats $CATS} ${MAXH:+--max_hours $MAXH} $EXTRA 2>&1 | grep -v Warning
done
