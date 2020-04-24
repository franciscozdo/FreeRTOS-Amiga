#include "tty.h"
#include "console.h"


#define EV_MAXNUM 16
#define KEY_EVENT 0x1
#define WRITE_EVENT 0x2
#define TTY_PRIORITY 3
#define CONSOLE_WIDTH 80
#define CONSOLE_HEIGHT 32

static TaskHandle_t TtyTask;
static QueueHandle_t KeyQueue;
static QueueHandle_t ReadQueue, WriteQueue;
static SemaphoreHandle_t WriteMtx;

static long TtyRead(File_t *f, char *buf, size_t nbyte);
static long TtyWrite(File_t *f, const char *buf, size_t nbyte);
static void TtyClose(File_t *f);
static void PushKeyEventFromISR(const KeyEvent_t *ev);

static FileOps_t TtyOps = {
  .read  = (FileRead_t) TtyRead,
  .write = (FileWrite_t)TtyWrite,
  .close = TtyClose
};

static char line[MAX_CANON + 1]; 
static size_t linepos = 0; 

static void sendLine(char *buf, size_t nbytes) {
  for (size_t i = 0; i < nbytes && i < 80; ++i) {
    printf("send to read %c\n", *(buf + i));
    xQueueSendToBack(ReadQueue, buf + i, 0);
  }
  char ch = '\0';
  xQueueSendToBack(ReadQueue, &ch, 0);
}

static void doDel() {
  if (linepos == 0)
    return;
  short x, y;
  ConsoleGetCursor(&x, &y);
  if (x == 0) {
    y--;
    x = 81;
  }
  linepos--;
  ConsoleSetCursor(x-1, y);
  ConsolePutChar(' ');
  ConsoleSetCursor(x-1, y);
}

static void eraseCurLine() {
  while (linepos > 0) {
    /* remove last char */
    doDel();
  }
  return;
}

static void doReturn() {
  if (linepos < 80)
    ConsolePutChar('\n');
  sendLine(line, linepos);
  linepos = 0;
}

static void doPrintable(char ch) {
  printf("got %c\n", ch);
  if (linepos >= 80) {
    return;
  }
  line[linepos++] = ch;
  ConsolePutChar(ch);
  short x, y;
  ConsoleGetCursor(&x, &y);
  printf("cursor %d %d\n", x, y);
}

static int handleKeyEvent(KeyEvent_t ev) {
  //static size_t posy = 0;
  if (ev.modifier & MOD_PRESSED) {
    if (ev.modifier & MOD_CONTROL) {
      switch (ev.code) {
        case KEY_U:
          /* kill line */
          printf("pressed ^U\n");
          eraseCurLine();
          break;
        default:
          break;
      }
      return 1;
    }
    
    if (ev.code == KEY_RETURN) {
      /* accept line and send to read */
      printf("pressed RETURN\n");
      doReturn();
      return 1;
    }
    if (ev.code ==  KEY_BACKSPACE) {
      /* remove last character */
      printf("pressed BACKSPACE\n");
      doDel();
      return 1;
    }

    if (ev.ascii >= 32 && ev.ascii <= 126) {
      /* printable character */
      doPrintable(ev.ascii);
      return 1;
    }
  }
  return 0;
}

static void handleWriteEvent() {
  xSemaphoreTake(WriteMtx, portMAX_DELAY);
  char ch;
  while (xQueueReceive(WriteQueue, (void *)&ch, 0) == pdTRUE) {
    ConsolePutChar(ch);
  }
  xSemaphoreGive(WriteMtx);
}

/* this procedure should be called in running task */
static void TtyThread(__unused void *data) {
  /* TODO: make buffer for whole screen */
  ConsoleDrawCursor();
  for (;;) {
    uint32_t notifyValue = 0;
    xTaskNotifyWait(0, KEY_EVENT | WRITE_EVENT, &notifyValue, portMAX_DELAY);

    ConsoleDrawCursor(); // unset cursor
    if (notifyValue & KEY_EVENT) {
      KeyEvent_t ev;
      while (xQueueReceive(KeyQueue, (void *)&ev, 0) == pdTRUE)
        handleKeyEvent(ev); 

    } else if (notifyValue & WRITE_EVENT) {
      printf("tty: got notify WRITE\n");
      handleWriteEvent();
    }
    ConsoleDrawCursor(); // set cursor back
  }
}

File_t *TtyOpen(void) {
  static File_t f = {.ops = &TtyOps};
  f.usecount++;
  if (f.usecount == 1) {
    printf("Init tty\n");
    /* TODO: synchronization */
    KeyboardInit(PushKeyEventFromISR);

    /* initialize all queues */
    KeyQueue = xQueueCreate(EV_MAXNUM, sizeof(KeyEvent_t));
    ReadQueue = xQueueCreate(MAX_CANON, sizeof(char));
    WriteQueue = xQueueCreate(MAX_WRITE, sizeof(char));


    WriteMtx = xSemaphoreCreateMutex();
    /* create task */
    xTaskCreate(TtyThread, "tty_driver", configMINIMAL_STACK_SIZE, NULL, 
                TTY_PRIORITY, &TtyTask);
  }
  return &f;
}

static void TtyClose(File_t *f) {
  f->usecount--;
  if (f->usecount == 0) {
    printf("Kill tty\n");
    /* TODO: synchronization */
    vQueueDelete(KeyQueue);
    vQueueDelete(ReadQueue);
    vQueueDelete(WriteQueue);
    KeyboardKill();
  }
}

static long TtyRead(__unused File_t *f, char *buf, size_t nbyte) {
  /* read from queue until \0 character */
  printf("read: %d bytes\n", nbyte);
  char ch;
  for(size_t i = 0; i < nbyte; ++i) {
    xQueueReceive(ReadQueue, (void *)&ch, portMAX_DELAY);
    if (ch == '\0') {
      printf("read completed (");
      for (size_t i = 0; i < nbyte; ++i)
        printf("%c", buf[i]);
      printf(")\n");
      return i;
    }
    buf[i] = ch;
  }

  printf("read completed (");
  for (size_t i = 0; i < nbyte; ++i)
    printf("%c", buf[i]);
  printf(")\n");

  return nbyte;
}

static long TtyWrite(__unused File_t *f, const char *buf, size_t nbyte) {
  xSemaphoreTake(WriteMtx, portMAX_DELAY);
  /* put buffer on queue and send notify to driver */
  printf("write: ");
  for (size_t i = 0; i < nbyte; ++i)
    printf("%c", buf[i]);
  printf("\n");
  xTaskNotify(TtyTask, WRITE_EVENT, eSetBits);
  for (size_t i = 0; i < nbyte; ++i) {
    if (xQueueSendToBack(WriteQueue, buf + i, portMAX_DELAY) == errQUEUE_FULL) {
      printf("write ended unexpectedly\n");
      xSemaphoreGive(WriteMtx);
      return i;
    }
  }
  printf("write done\n");
  xSemaphoreGive(WriteMtx);
  return nbyte;
}

static void PushKeyEventFromISR(const KeyEvent_t *ev) {
  xQueueSendToBackFromISR(KeyQueue, (const void *)ev, &xNeedRescheduleTask);
  //BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  xTaskNotifyFromISR(TtyTask, KEY_EVENT, eSetBits, &xNeedRescheduleTask);
}
