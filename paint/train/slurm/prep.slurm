#!/bin/bash
#SBATCH --account=def-jcaplan
#SBATCH --job-name=paint_prep
#SBATCH --output=/scratch/devon7y/macplus_paint/logs/%x_%j.out
#SBATCH --error=/scratch/devon7y/macplus_paint/logs/%x_%j.err
#SBATCH --time=01:00:00
#SBATCH --nodes=1
#SBATCH --cpus-per-task=64
#SBATCH --mem=180G
module load StdEnv/2023 python/3.12
cd /scratch/devon7y/macplus_paint
source venv/bin/activate
python -I code/prep_data.py data/npz data/prep
