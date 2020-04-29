#include <FreeRTOS/FreeRTOS.h>
#include <FreeRTOS/task.h>

#include <custom.h>
#include <trap.h>
#include <stdio.h>

extern int rand(void);

static void vMainTask(__unused void *data) {
  for (;;) {
    custom.color[0] = rand();
  }
}

static xTaskHandle handle;

extern void vPortDefaultTrapHandler(struct TrapFrame *);

/* TODO: use C bit fields to define instruction encoding */
typedef struct MulsInst {
} MulsInst_t;

typedef struct DivsInst {
} DivsInst_t;

/* Keep in mind that:
 * - PC & SR are placed at different positions in the trap frame
 *   for 68000 and 68010 processors
 * - gcc replaces 32-bit multiplication and division by calls to
 *   __mulsi3 and __divsi3 procedures
 */
static bool EmulMuls(struct TrapFrame *frame) {
  /* TODO: implement muls.l instruction emulation */
  (void)frame;
  return false;
}

static bool EmulDivs(struct TrapFrame *frame) {
  /* TODO: implement divsl.l instruction emulation */
  (void)frame;
  return false;
}

void vPortTrapHandler(struct TrapFrame *frame) {
  /* TODO: implement muls.l and divsl.l instruction emulation. */
  if (frame->trapnum == T_ILLINST) {

    uint32_t *addr = (uint32_t *)frame->m68000.pc;
    uint32_t instr = *addr;
    uint32_t data0 = *(addr + 1);
    //printf("hexdump 0x%x: %x %x\n", addr, instr, data0);
#define DIVLL 0x4c7c0800
#define MULSL 0x4c3c0800

    if ((instr & DIVLL) == DIVLL) {
      int dr = instr & 0x7;
      int dq = (instr >> 12) & 0x7;
      uint32_t *regs = (uint32_t*)frame;

      int32_t divident = regs[dq];
      int32_t divider = data0;

      if (divider == 0) {
        frame->trapnum = T_ZERODIV;
        vPortDefaultTrapHandler(frame);
      }
      int32_t res = divident / divider;
      int32_t rem = divident - (divider * res);

      /* set answear */
      regs[dq] = res;
      regs[dr] = rem;

      /* set sr */
      uint16_t *sr = &(frame->m68000.sr);
      *sr = *sr & 0xFFF0;

      /* negative */
      if (res < 0)
        *sr |= 0x8;

      /* zero */
      if (res == 0)
        *sr |= 0x4;

      /* overflow and carry not affected */

      /* move pc to next instr */
      frame->m68000.pc += 8;

      return;
    } else if ((instr & MULSL) == MULSL) {
      int dl = (instr  >> 12) & 0x7;
      //printf("Detected instruction muls.l #%d, d%d\n", data0, dl);
      uint32_t *regs = (uint32_t*)frame;
      //printf("d%d: %d\n", dl, regs[dl]);

      int64_t mult1 = (int64_t) data0;
      int64_t mult2 = (int64_t) regs[dl];

      int minus = ((mult1 < 0) && (mult2 > 0)) || ((mult1 > 0) && (mult2 < 0));

      if (mult1 < 0)
        mult1 = -mult1;

      if (mult2 < 0)
        mult2 = -mult2;

      int64_t prod = 0;
      while (mult1) {
        if (mult1 & 1)
          prod += mult2;
        mult2 <<= 1;
        mult1 >>= 1;
      }

      int ov = (prod & 0xFFFFFFFF00000000) != 0;

      if (minus)
        prod = -prod;
      /* set answear */
      regs[dl] = (int32_t) prod;

      /* set sr */
      uint16_t *sr = &(frame->m68000.sr);
      *sr = *sr & 0xFFF0;

      /* negative */
      if (prod < 0)
        *sr |= 0x8;

      /* zero */
      if (prod == 0)
        *sr |= 0x4;

      /* overflow */
      if (ov)
        *sr |= 0x2;

      /* carry not affected */

      /* move pc to next instr */
      frame->m68000.pc += 8;
      return;
    }
  }
  if (EmulMuls(frame) || EmulDivs(frame))
    return;

  custom.color[0] = 0xf00;
  vPortDefaultTrapHandler(frame);
}

int main(void) {
  portNOP(); /* Breakpoint for simulator. */

  xTaskCreate(vMainTask, "main", configMINIMAL_STACK_SIZE, NULL, 0, &handle);

  vTaskStartScheduler();

  return 0;
}

void vApplicationIdleHook(void) { custom.color[0] = 0x00f; }
