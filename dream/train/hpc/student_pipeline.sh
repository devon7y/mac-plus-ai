#!/bin/bash
# Student pipeline in one job: [teacher sheets] -> float distillation -> QAT -> evaluation sheets.
# Run through job.slurm:  sbatch ... job.slurm bash hpc/student_pipeline.sh
# Environment: T (teacher ema.pt), O (output dir), DATA (split or synth:N), and optionally
#   TEACHER_SHEET=1 (all-subject w=3 sheets of T first), FLOAT_ITERS, QAT_ITERS, QAT_LR, FLOAT_ARGS,
#   QAT_ARGS, EVAL_ARGS, SKIP_FLOAT=1, COMMON_ARGS (passed to both distill stages),
#   FLOAT_INIT=<float.pt> (continue float training from that checkpoint instead of calibrating anew)
set -uo pipefail
T=${T:?teacher}; O=${O:?out}; DATA=${DATA:?data}
mkdir -p $O
if [ -n "${TEACHER_SHEET:-}" ]; then
  python -u teacher_sheet.py $T --out $O/teacher_sheets --w 3 --seeds 6 || true
fi
if [ -z "${SKIP_FLOAT:-}" ]; then
  python -u distill.py --teacher $T --out $O --stage float --iters ${FLOAT_ITERS:-20000} --data $DATA \
    --bank_file ${BANK:-$O/bank.pt} ${FLOAT_INIT:+--init $FLOAT_INIT} ${COMMON_ARGS:-} ${FLOAT_ARGS:-} || exit 1
fi
python -u distill.py --teacher $T --out $O --stage qat --init $O/float.pt --iters ${QAT_ITERS:-10000} \
  --data $DATA --bank_file ${BANK:-$O/bank.pt} --lr ${QAT_LR:-2e-4} ${COMMON_ARGS:-} ${QAT_ARGS:-} || exit 1
python -u evaluate.py --teacher $T --float $O/float.pt --qat $O/qat.pt --data $DATA --out $O/eval ${EVAL_ARGS:-}
python -u plot_logs.py --student $O --out $O/eval/curves_student.png || true
