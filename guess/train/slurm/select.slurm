#!/bin/bash
# Category selection from an all-345 checkpoint: sbatch --export=ALL,CKPT=../runs/fpC/ckpt.pt,OUT=../runs/select_C select.slurm
#SBATCH --account=def-jcaplan
#SBATCH --job-name=guess_select
#SBATCH --output=/home/devon7y/macplus_guess/logs/%x_%j.out
#SBATCH --error=/home/devon7y/macplus_guess/logs/%x_%j.err
#SBATCH --time=00:30:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=32G
#SBATCH --gpus=h100:1
module load StdEnv/2023 python/3.12
source /scratch/devon7y/macplus_paint/venv/bin/activate
cd /home/devon7y/macplus_guess/code
python select_cats.py --prep /scratch/devon7y/macplus_paint/data/prep --ckpt $CKPT --out $OUT --per 1000 --exclude exclude.txt 2>&1 | grep -v Warning
