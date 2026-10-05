.section .text
.global __start
.type __start, STT_FUNC
__start:
    R0 = 0;
    R1 = 0;
    R2 = 0;
    R3 = 0;
    R4 = 0;
    R5 = 0;
    R6 = 0;
    R7 = 0;
    P0 = 0;
    P1 = 0;
    P2 = 0;
    P3 = 0;
    P4 = 0;
    P5 = 0;

    R0.H = 0x1234;
    R0.L = 0x5678;
    R1 = 7;
    R1 += 5;

    P1.H = result;
    P1.L = result;
    [P1] = R0;
    R2 = [P1];
    R3 = R2 + R1;
    [P1 + 4] = R3;
    R5 = [P1 + 4];

    P0.H = exit_params;
    P0.L = exit_params;
    [P0] = R4;
    R0 = P0;
    P0 = 1;
    EXCPT 0;

.section .data
.align 4
.global result
result:
    .long 0
    .long 0
exit_params:
    .long 0
    .long 0
    .long 0
