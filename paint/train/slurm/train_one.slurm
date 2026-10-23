#!/bin/bash
# One model per job (short jobs backfill quickly on Rorqual), then its evaluation.
# Usage: sbatch --job-name=paint_h256_tern --export=ALL,H=256,Q=tern,STEPS=300000[,TAG=..,INIT=..] train_one.slurm
#SBATCH --account=def-jcaplan
#SBATCH --output=/scratch/devon7y/macplus_paint/logs/%x_%j.out
#SBATCH --error=/scratch/devon7y/macplus_paint/logs/%x_%j.err
#SBATCH --time=02:15:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=48G
#SBATCH --gpus=h100:1
module load StdEnv/2023 python/3.12
cd /scratch/devon7y/macplus_paint
source venv/bin/activate
cd code
NAME=h${H}_${Q}${TAG}
EXTRA=""
[ -n "$INIT" ] && EXTRA="--init $INIT"
[ -n "$TEACHER" ] && EXTRA="$EXTRA --teacher $TEACHER ${DISTILL:+--distill $DISTILL} ${DISTILL_H:+--distill_h $DISTILL_H}"
python train.py --prep ../data/prep --out ../runs/$NAME --H $H --quant $Q --steps $STEPS \
   --max_hours ${MAXH:-1.75} ${LR:+--lr $LR} ${WARM:+--warmup $WARM} $EXTRA 2>&1 | grep -v "UserWarning\|_VF.gru"
if [ -f ../runs/$NAME/final.pt ]; then CK=../runs/$NAME/final.pt; else CK=../runs/$NAME/ckpt.pt; fi
python evaluate.py --prep ../data/prep --ckpt $CK --out ../eval/$NAME 2>&1 | grep -v "UserWarning\|_VF.gru"
