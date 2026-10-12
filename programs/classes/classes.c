// What each kind of instruction costs the machine: loops of one kind each, a fixed number of
// instructions long, with a mark on the console between them. The harness times from mark to
// mark (rvc_harness --image classes --stats-from P2 --until P3: instructions a second for
// phase 2). Bare metal, machine mode, no paging.
//
//   0  arithmetic: 15 register operations and a branch back (a new instruction texel each trip)
//   1  loads: 14 lw from one page and a branch back
//   2  stores: 14 sw to four texels (a new value each trip) and a branch back
//   3  a two-instruction loop within one texel (no fetch at all)
//   4  call and return: jal to a function a page away, ret
//   5  loads that stride through 4 MB (every one a texel the card has not seen lately)
//   6  taken branches forward within a page, every other instruction
#include "../common/platform.h"

static uint32_t data[64] __attribute__((aligned(64)));
static uint32_t far_data[1 << 20] __attribute__((aligned(64)));

__attribute__((noinline, aligned(4096))) uint32_t far_leaf(uint32_t x) { return x + 1; }

static void mark(int k) {
    uart_putc('P');
    uart_putc('0' + k);
    uart_putc('\n');
}

#define TRIPS(per_trip) (6000000u / (per_trip))

int main(void) {
    uint32_t n, *p = data;
    mark(0);
    n = TRIPS(17);
    __asm__ volatile(
        ".balign 16\n"
        "1: addi t1, t1, 1\n xor t2, t2, t1\n add t3, t3, t2\n slli t4, t3, 3\n"
        "   sub t5, t4, t1\n or t6, t5, t2\n andi t1, t1, 0x7ff\n srli t2, t6, 2\n"
        "   add t3, t3, t5\n xori t4, t4, 0x55\n sltu t5, t3, t4\n add t6, t6, t5\n"
        "   slli t1, t1, 1\n srai t2, t2, 1\n and t3, t3, t6\n"
        "   addi %0, %0, -1\n bnez %0, 1b\n" : "+r"(n) : : "t1", "t2", "t3", "t4", "t5", "t6");
    mark(1);
    n = TRIPS(16);
    __asm__ volatile(
        ".balign 16\n"
        "1: lw t1, 0(%1)\n lw t2, 4(%1)\n lw t3, 8(%1)\n lw t4, 12(%1)\n"
        "   lw t5, 16(%1)\n lw t6, 20(%1)\n lw t1, 24(%1)\n lw t2, 28(%1)\n"
        "   lw t3, 32(%1)\n lw t4, 36(%1)\n lw t5, 40(%1)\n lw t6, 44(%1)\n"
        "   lw t1, 48(%1)\n lw t2, 52(%1)\n"
        "   addi %0, %0, -1\n bnez %0, 1b\n" : "+r"(n) : "r"(p) : "t1", "t2", "t3", "t4", "t5", "t6");
    mark(2);
    n = TRIPS(16);
    __asm__ volatile(
        ".balign 16\n"
        "1: sw %0, 0(%1)\n sw %0, 4(%1)\n sw %0, 8(%1)\n sw %0, 12(%1)\n"
        "   sw %0, 16(%1)\n sw %0, 20(%1)\n sw %0, 24(%1)\n sw %0, 28(%1)\n"
        "   sw %0, 32(%1)\n sw %0, 36(%1)\n sw %0, 40(%1)\n sw %0, 44(%1)\n"
        "   sw %0, 48(%1)\n sw %0, 52(%1)\n"
        "   addi %0, %0, -1\n bnez %0, 1b\n" : "+r"(n) : "r"(p) : "memory");
    mark(3);
    n = TRIPS(2);
    __asm__ volatile(
        ".balign 16\n"
        "1: addi %0, %0, -1\n bnez %0, 1b\n" : "+r"(n));
    mark(4);
    n = TRIPS(5);
    {
        uint32_t x = 0;
        for (uint32_t i = 0; i < n; i++) x = far_leaf(x);   // jal, addi, ret, and the loop's two
        data[0] = x;
    }
    mark(5);
    n = TRIPS(4);
    {
        uint32_t at = 0, sum = 0;
        __asm__ volatile(
            ".balign 16\n"
            "1: add t1, %2, %1\n lw t2, 0(t1)\n add %3, %3, t2\n addi %1, %1, 1040\n"
            "   and %1, %1, %4\n addi %0, %0, -1\n bnez %0, 1b\n"
            : "+r"(n), "+r"(at), "+r"(p), "+r"(sum) : "r"(0x3ffffcu) : "t1", "t2");
        (void)far_data;
        data[1] = sum;
    }
    mark(6);
    n = TRIPS(8);
    __asm__ volatile(
        ".balign 16\n"
        "1: j 2f\n nop\n nop\n nop\n nop\n"
        "2: addi t1, t1, 1\n j 3f\n nop\n nop\n nop\n nop\n nop\n"
        "3: addi t2, t2, 1\n j 4f\n nop\n nop\n nop\n nop\n nop\n nop\n"
        "4: addi t3, t3, 1\n addi %0, %0, -1\n bnez %0, 1b\n" : "+r"(n) : : "t1", "t2", "t3");
    mark(7);
    for (;;) __asm__ volatile("wfi");
}
