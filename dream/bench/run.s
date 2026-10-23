| Dream: position loop for generated layer code (see dream/engine/jit.py).
| void k_run(const void *code, void *a0, void *a1, const short *bias, const short *tabmid,
|            long nx, long ny, long in_xstep, long in_rowstep, long out_xstep, long out_rowstep)
| Calls code once per grid position with a0/a1 = the position's input/output base, a2 = bias,
| a3 = table middle. The generated code uses d0-d7/a0-a3 and restores a0/a1.
        .text
        .globl  k_run
k_run:  movem.l %d2-%d7/%a2-%a6,-(%sp)
        lea     48(%sp),%a4             | args
        movea.l (%a4),%a6               | code
        movea.l 4(%a4),%a0
        movea.l 8(%a4),%a1
        move.l  24(%a4),-(%sp)          | rows left
1:      move.l  20(%a4),-(%sp)          | columns left
2:      movea.l 12(%a4),%a2
        movea.l 16(%a4),%a3
        jsr     (%a6)
        adda.l  28(%a4),%a0
        adda.l  36(%a4),%a1
        subq.l  #1,(%sp)
        bne.s   2b
        addq.l  #4,%sp
        adda.l  32(%a4),%a0
        adda.l  40(%a4),%a1
        subq.l  #1,(%sp)
        bne.s   1b
        addq.l  #4,%sp
        movem.l (%sp)+,%d2-%d7/%a2-%a6
        rts
