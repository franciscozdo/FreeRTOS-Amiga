#include <FreeRTOS/FreeRTOS.h>
#include <FreeRTOS/task.h>

#include <interrupt.h>
#include <stdio.h>

#include <copper.h>
#include <palette.h>
#include <bitmap.h>
#include <sprite.h>
#include <font.h>
#include <file.h>
#include <stdio.h>

#include "console.h"
#include "tty.h"
#include "data/lat2-08.c"
#include "data/screen.c"
#include "data/pointer.c"

static COPLIST(cp, 40);

#define mainINPUT_TASK_PRIORITY 2

int atoh(char *buf, int *offset) {
  int i = *offset;
  int r = 0;
  while ((buf[i] >= '0' && buf[i] <= '9') || (buf[i] >= 'a' && buf[i] <= 'f')) {
    r *= 16;
    if (buf[i] >= '0' && buf[i] <= '9') 
      r += buf[i] - '0';
    else if (buf[i] >= 'a' && buf[i] <= 'f') 
      r += buf[i] - 'a' + 10;
    i++;
  }
  *offset = i;
  return r;
}

void parseCommand(char *buf, int len, char *c, uint32_t *a, uint32_t *b) {
  int off = 0;
  *c = buf[0];
  off += 2;
  *a = atoh(buf, &off);
  off++;
  *b = atoh(buf, &off);
}

char *bytetochar(uint8_t b) {
  static char s[2];
  uint8_t b0 = (b >> 4) & 0xF;
  uint8_t b1 = b & 0xF;
  if (b0 < 0xA)
    s[0] = '0' + b0;
  else
    s[0] = 'a' + b0 - 10;

  if (b1 < 0xA)
    s[1] = '0' + b1;
  else
    s[1] = 'a' + b1 - 10;
  return s;
}

void dumpMemory(File_t *tty, void *start, void *end) {
  while (start <= end) {
    FileWrite(tty, " ", 1);
    FileWrite(tty, bytetochar(*(uint8_t *)start), 2);
    start++;
  }
}

static void vInputTask(void *data) {
  File_t *tty = data;
  FileWrite(tty, "Hello! Start writing commands.\n", 31);
  for (;;) {
    static char buf[1024];
    int r = FileRead(tty, buf, 1024);
    if (r == 0) continue;

    char command;
    uint32_t arg1, arg2;
    parseCommand(buf, r, &command, &arg1, &arg2);
    printf("command: %c, %d %d\n", command, arg1, arg2);

    if (command == 'c') {
      CopLoadColor(cp, arg1, 
          ((arg2 & 0xf) << 0) | (arg2 & 0xf0) | ((arg2 & 0xf00) >> 0));
    } else if (command == 'd') {
      FileWrite(tty, "memory dump: ", 12);
      dumpMemory(tty, arg1 - (arg1 % 2), arg2 + arg2 % 2);
      FileWrite(tty, "\n", 1);
    } else if (command == 'e') {
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
  EnableDMA(DMAF_RASTER|DMAF_SPRITE);

  ConsoleInit(&screen_bm, &console_font);


  xTaskCreate(vInputTask, "input", configMINIMAL_STACK_SIZE, TtyOpen(),
              mainINPUT_TASK_PRIORITY, &input_handle);

  vTaskStartScheduler();

  return 0;
}

void vApplicationIdleHook(void) {}
