#!/bin/bash
# Teacher -> student (float, QAT) -> evaluation in one job (saves queue waits). Run through job.slurm:
#   sbatch ... job.slurm bash hpc/full_pipeline.sh
# Environment: R (run dir; teacher in $R/teacher, student in $R/student), DATA, TEACHER_ITERS,
#   TEACHER_ARGS, plus everything student_pipeline.sh reads (FLOAT_ITERS, QAT_ITERS, FLOAT_ARGS, ...).
set -uo pipefail
R=${R:?run dir}; DATA=${DATA:?data}
mkdir -p $R
python -u train_teacher.py --data $DATA --out $R/teacher --iters ${TEACHER_ITERS:-10000} ${TEACHER_ARGS:-} || exit 1
python -u plot_logs.py --teacher $R/teacher/log.jsonl --out $R/teacher/curves.png 2>/dev/null || true
T=$R/teacher/ema.pt O=$R/student DATA=$DATA bash hpc/student_pipeline.sh || exit 1
