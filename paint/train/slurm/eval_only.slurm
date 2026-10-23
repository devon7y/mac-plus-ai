#!/bin/bash
# Re-run evaluate.py on an existing checkpoint. Usage:
#   sbatch --export=ALL,CK=../runs/h384_tern_ft/final.pt,NAME=h384_tern_ft[,ONLY_GRIDS=1] eval_only.slurm
#SBATCH --account=def-jcaplan
#SBATCH --job-name=paint_eval
#SBATCH --output=/scratch/devon7y/macplus_paint/logs/%x_%j.out
#SBATCH --error=/scratch/devon7y/macplus_paint/logs/%x_%j.err
#SBATCH --time=00:30:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=48G
#SBATCH --gpus=h100:1
module load StdEnv/2023 python/3.12
cd /scratch/devon7y/macplus_paint
source venv/bin/activate
cd code
python evaluate.py --prep ../data/prep --ckpt $CK --out ../eval/$NAME ${ONLY_GRIDS:+--only_grids} 2>&1 | grep -v "UserWarning\|_VF.gru"
