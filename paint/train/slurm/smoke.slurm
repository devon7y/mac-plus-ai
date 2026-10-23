#!/bin/bash
#SBATCH --account=def-jcaplan
#SBATCH --job-name=paint_smoke
#SBATCH --output=/scratch/devon7y/macplus_paint/logs/%x_%j.out
#SBATCH --error=/scratch/devon7y/macplus_paint/logs/%x_%j.err
#SBATCH --time=00:40:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=6
#SBATCH --mem=64G
#SBATCH --gpus=h100:1
module load StdEnv/2023 python/3.12
cd /scratch/devon7y/macplus_paint
source venv/bin/activate
cd code
nvidia-smi --query-gpu=name,memory.total --format=csv
rm -rf ../runs/smoke_*
python train.py --prep ../data/prep --out ../runs/smoke_h384_tern --H 384 --quant tern --steps 1200 --eval_every 600 --val_n 3450 --warmup 200
python train.py --prep ../data/prep --out ../runs/smoke_h192_fp --H 192 --quant fp --steps 600 --eval_every 600 --val_n 3450 --warmup 200
python evaluate.py --prep ../data/prep --ckpt ../runs/smoke_h384_tern/final.pt --out ../eval/smoke_h384_tern --sim_n 3450
