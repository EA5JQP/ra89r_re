#include "driver/fault.h"

#include "py32f4xx.h"
#include "driver/uart.h"

/* Exception stack frame layout (see the ARMv7-M ARM): r0, r1, r2, r3, r12, lr,
 * pc, xpsr.  For a fault escalated to HardFault there may be extra words, but
 * the first eight are always in this order. */
#define FRAME_R0   0
#define FRAME_R1   1
#define FRAME_R2   2
#define FRAME_R3   3
#define FRAME_R12  4
#define FRAME_LR   5
#define FRAME_PC   6
#define FRAME_PSR  7

static const char *mmfsr_text(uint32_t bits)
{
    switch (bits & 0xFFu) {
    case 0x01u: return "instruction access violation";
    case 0x02u: return "data access violation";
    case 0x80u: return "stacking/unstacking error";
    default:    return "memmanage";
    }
}

static const char *bfsr_text(uint32_t bits)
{
    uint32_t b = bits & 0xFFu;

    if (b & 0x80u) return "stacking/unstacking error";
    if (b & 0x10u) return "precise data bus error";
    if (b & 0x04u) return "imprecise data bus error";
    if (b & 0x02u) return "instruction bus error";
    if (b & 0x01u) return "vector table read error";
    return "bus fault";
}

static const char *ufsr_text(uint32_t bits)
{
    uint32_t b = (bits >> 16) & 0xFFFFu;

    if (b & 0x0200u) return "divide by zero";
    if (b & 0x0100u) return "unaligned access";
    if (b & 0x0008u) return "no coprocessor";
    if (b & 0x0004u) return "invalid PC load";
    if (b & 0x0002u) return "invalid state";
    if (b & 0x0001u) return "undefined instruction";
    return "usage fault";
}

void fault_report(uint32_t *frame, uint32_t exc_return)
{
    uint32_t cfsr = SCB->CFSR;

    uart_puts("\n\n*** CPU FAULT ***\n");
    if (cfsr & 0x000000FFu) {
        uart_printf("  memory management: %s (MMFSR=%02X, MMFAR=%08X)\n",
                    mmfsr_text(cfsr), (unsigned)(cfsr & 0xFFu),
                    (unsigned)SCB->MMFAR);
    }
    if (cfsr & 0x0000FF00u) {
        uart_printf("  bus fault: %s (BFSR=%02X, BFAR=%08X)\n",
                    bfsr_text(cfsr >> 8), (unsigned)((cfsr >> 8) & 0xFFu),
                    (unsigned)SCB->BFAR);
    }
    if (cfsr & 0xFFFF0000u) {
        uart_printf("  usage fault: %s (UFSR=%04X)\n",
                    ufsr_text(cfsr), (unsigned)(cfsr >> 16));
    }
    if (SCB->HFSR & 0x40000000u)
        uart_puts("  escalated to HardFault (HFSR.FORCED)\n");

    uart_printf("  HFSR=%08X EXC_RETURN=%08X\n", (unsigned)SCB->HFSR,
                (unsigned)exc_return);
    uart_printf("  r0=%08X r1=%08X r2=%08X r3=%08X\n",
                (unsigned)frame[FRAME_R0], (unsigned)frame[FRAME_R1],
                (unsigned)frame[FRAME_R2], (unsigned)frame[FRAME_R3]);
    uart_printf("  r12=%08X lr=%08X pc=%08X psr=%08X\n",
                (unsigned)frame[FRAME_R12], (unsigned)frame[FRAME_LR],
                (unsigned)frame[FRAME_PC], (unsigned)frame[FRAME_PSR]);
    uart_puts("  halted -- please report the pc value; power-cycle the radio\n");

    for (;;)
        __NOP();
}

/* Naked trampolines: pick the frame the CPU pushed (MSP or PSP, decided by bit
 * 2 of EXC_RETURN) and hand it to fault_report together with EXC_RETURN. */
#define FAULT_HANDLER(name)                                                   \
    __attribute__((naked)) void name(void)                                    \
    {                                                                         \
        __asm volatile("tst lr, #4        \n"                                 \
                       "ite eq           \n"                                  \
                       "mrseq r0, msp    \n"                                  \
                       "mrsne r0, psp    \n"                                  \
                       "mov r1, lr       \n"                                  \
                       "b fault_report   \n");                                \
    }

FAULT_HANDLER(NMI_Handler)
FAULT_HANDLER(HardFault_Handler)
FAULT_HANDLER(MemManage_Handler)
FAULT_HANDLER(BusFault_Handler)
FAULT_HANDLER(UsageFault_Handler)
