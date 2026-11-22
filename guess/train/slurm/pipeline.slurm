#!/bin/bash
# Float training, QAT fine-tune, export to a Mac model file and the integer model's accuracy.
# sbatch --job-name=guess_NAME --export=ALL,NAME=..,ARCH=..,CATS=..,FP=120000,Q=60000[,INIT=..,LR=..] pipeline.slurm
#SBATCH --account=def-jcaplan
#SBATCH --output=/home/devon7y/macplus_guess/logs/%x_%j.out
#SBATCH --error=/home/devon7y/macplus_guess/logs/%x_%j.err
#SBATCH --time=03:00:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=64G
#SBATCH --gpus=h100:1
module load StdEnv/2023 python/3.12
source /scratch/devon7y/macplus_paint/venv/bin/activate
cd /home/devon7y/macplus_guess/code
P=/scratch/devon7y/macplus_paint/data/prep
R=../runs/$NAME
if [ -z "$INIT" ]; then
  python train.py --prep $P --arch "$ARCH" --cats $CATS --steps ${FP:-120000} --out ${R}_fp ${FRAC:+--frac_full $FRAC} 2>&1 | grep -v Warning
  INIT=${R}_fp/ckpt.pt
fi
python train.py --prep $P --arch "$ARCH" --cats $CATS --steps ${Q:-60000} --qat --init $INIT --lr ${LR:-1e-3} \
   --out ${R}_q ${FRAC:+--frac_full $FRAC} ${KD:+--teacher $INIT --kd $KD} ${AFULL:+--afull} 2>&1 | grep -v Warning
python export.py ${R}_q/ckpt.pt ${R}_q/model.gmdl --prep $P --eval 0 2>&1 | grep -v Warning
