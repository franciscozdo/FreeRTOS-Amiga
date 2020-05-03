#include <FreeRTOS/FreeRTOS.h>
#include <FreeRTOS/task.h>

#include <interrupt.h>
#include <stdio.h>
#include <stdlib.h>

#include <copper.h>
#include <palette.h>
#include <bitmap.h>
#include <sprite.h>
#include <font.h>
#include <file.h>

#include "console.h"
#include "tty.h"
#include "data/lat2-08.c"
#include "data/screen.c"
#include "data/pointer.c"

static COPLIST(cp, 40);

#define mainINPUT_TASK_PRIORITY 2

static void vInputTask(void *data) {
  File_t *tty = data;
  FileWrite(tty, "Hello! Start writing commands.\n", 31);
  for (;;) {
    static char buf[1024];
    int r = FileRead(tty, buf, 1024);
    if (r == 0)
      continue;

    char command = buf[0];

    if (command == 'c') {
      char *next;
      uint32_t arg1, arg2;
      arg1 = strtoul(buf + 2, &next, 16);
      arg2 = strtoul(next, NULL, 16);

      CopLoadColor(cp, arg1, arg2);
    } else if (command == 'd') {
      char *next;
      uint32_t arg1, arg2;
      arg1 = strtoul(buf + 2, &next, 16);
      arg2 = strtoul(next, NULL, 16);

      FileHexDump(tty, (void *)arg1, arg2 - arg1);
    } else if (command == 'e') {
      if (r <= 2)
        continue;
      FileWrite(tty, buf + 2, r - 2);
      FileWrite(tty, "\n", 1);
    } else {
      FileWrite(tty, "unknown command\n", 16);
    }
  }
}

static void SystemClockTickHandler(__unused void *data) {
  /* Increment the system timer value and possibly preempt. */
  uint32_t ulSavedInterruptMask = portSET_INTERRUPT_MASK_FROM_ISR();
  xNeedRescheduleTask = xTaskIncrementTick();
  portCLEAR_INTERRUPT_MASK_FROM_ISR(ulSavedInterruptMask);
}

INTSERVER_DEFINE(SystemClockTick, 10, SystemClockTickHandler, NULL);

static xTaskHandle input_handle;

int main(void) {
  portNOP(); /* Breakpoint for simulator. */

  /* Configure system clock. */
  AddIntServer(VertBlankChain, SystemClockTick);

  /*
   * Copper configures hardware each frame (50Hz in PAL) to:
   *  - set video mode to HIRES (640x256),
   *  - display one bitplane,
   *  - set background color to black, and foreground to white,
   *  - set up mouse pointer palette,
   *  - set sprite 0 to mouse pointer graphics,
   *  - set other sprites to empty graphics,
   */
  CopSetupScreen(cp, &screen_bm, MODE_HIRES, HP(0), VP(0));
  CopSetupBitplanes(cp, &screen_bm, NULL);
  CopLoadColor(cp, 0, 0x123);
  CopLoadColor(cp, 1, 0x0c3);
  CopLoadPal(cp, &pointer_pal, 16);

  /* Tell copper where the copper list begins and enable copper DMA. */
  CopListActivate(cp);

  /* Enable bitplane and sprite fetchers' DMA. */
  EnableDMA(DMAF_RASTER | DMAF_SPRITE);

  ConsoleInit(&screen_bm, &console_font);

  xTaskCreate(vInputTask, "input", configMINIMAL_STACK_SIZE, TtyOpen(),
              mainINPUT_TASK_PRIORITY, &input_handle);

  vTaskStartScheduler();

  return 0;
}

void vApplicationIdleHook(void) {
}
