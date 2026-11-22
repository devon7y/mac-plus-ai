#!/bin/bash
# The shipped model's accuracy on the held-out test split, in the Mac's integer arithmetic (export.py's
# batched integer model, checked bit-exact against ref.py), and the float model's for comparison.
#   sbatch --export=ALL,MODEL=../runs/C2_150_q/model.gmdl,FP=../runs/C2_150kd_fp/ckpt.pt eval_test.slurm
#SBATCH --account=def-jcaplan
#SBATCH --job-name=guess_test
#SBATCH --output=/home/devon7y/macplus_guess/logs/%x_%j.out
#SBATCH --time=00:30:00
#SBATCH --cpus-per-task=4
#SBATCH --mem=32G
#SBATCH --gpus=h100:1
module load StdEnv/2023 python/3.12
source /scratch/devon7y/macplus_paint/venv/bin/activate
cd /home/devon7y/macplus_guess/code
P=/scratch/devon7y/macplus_paint/data/prep
python export.py x $MODEL --no-write --prep $P --eval 0 --split test --check 64 --accstats 2>&1 | grep -v Warning
GUESS_SPLIT=test python train.py --prep $P --arch t16_p_c32k3s2_c64k3s2_c128k3s2_f_l384_o --cats cats_150.txt \
  --out ../runs/fp_test_eval --init $FP --eval_only 2>&1 | grep -E "RESULT"
