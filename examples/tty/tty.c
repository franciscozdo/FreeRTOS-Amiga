#include "tty.h"
#include "console.h"

#define EV_MAXNUM 16
#define KEY_EVENT 0x1
#define WRITE_EVENT 0x2
#define TTY_PRIORITY 2
#define MAX_CANON 80
#define MAX_WRITE 1024 // write buffer

static TaskHandle_t TtyTask;
static QueueHandle_t KeyQueue;
static QueueHandle_t ReadQueue, WriteQueue;

struct CurrWrite {
  TaskHandle_t task;
  size_t toRead;
};

/* Queue to synchronize writes (something like semaphore) */
static QueueHandle_t WriteWaitQueue;

static long TtyRead(File_t *f, char *buf, size_t nbyte);
static long TtyWrite(File_t *f, const char *buf, size_t nbyte);
static void TtyClose(File_t *f);
static void PushKeyEventFromISR(const KeyEvent_t *ev);

static FileOps_t TtyOps = {.read = (FileRead_t)TtyRead,
                           .write = (FileWrite_t)TtyWrite,
                           .close = TtyClose};

static char line[MAX_CANON + 1];
static size_t linepos = 0;

static void sendLine(char *buf, size_t nbytes) {
  /* push every character from buffer on queue */
  for (size_t i = 0; i < nbytes && i < 80; ++i) {
    xQueueSendToBack(ReadQueue, buf + i, 0);
  }

  /* send EOT on queue */
  char ch = '\0';
  xQueueSendToBack(ReadQueue, &ch, 0);
}

static void doDel() {
  if (linepos == 0)
    /* you can't remove when you don't typed anything */
    return;
  short x, y;
  ConsoleGetCursor(&x, &y);
  if (x == 0) {
    /* cursor is in new line */
    y--;
    x = 81;
  }

  /* remove from buffer */
  linepos--;

  /* remove from screen */
  ConsoleSetCursor(x - 1, y);
  ConsolePutChar(' ');
  ConsoleSetCursor(x - 1, y);
}

static void eraseCurLine() {
  while (linepos > 0) {
    /* remove last char */
    doDel();
  }
  return;
}

static void doReturn() {
  short x, y;
  ConsoleGetCursor(&x, &y);
  if (x != 0 || linepos == 0)
    /* if x == 0 and linepos > 0 than cursor was moved automatically */
    ConsolePutChar('\n');
  sendLine(line, linepos);
  linepos = 0;
}

static void doPrintable(char ch) {
  if (linepos >= 80) {
    /* You can't write more than 80 characters */
    return;
  }
  line[linepos++] = ch;
  ConsolePutChar(ch);
}

static int handleKeyEvent(KeyEvent_t ev) {
  if (ev.modifier & MOD_PRESSED) {
    if (ev.modifier & MOD_CONTROL) {
      switch (ev.code) {
        case KEY_U:
          /* kill line */
          eraseCurLine();
          break;
        default:
          /* we don't have other actions with ctrl */
          break;
      }
      return 1;
    }

    if (ev.code == KEY_RETURN) {
      /* accept line and send to read */
      doReturn();
      return 1;
    }
    if (ev.code == KEY_BACKSPACE) {
      /* remove last character */
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
  /* print all characters from queue */
  /* there is (probably) something to read */
  char ch;
  while (xQueueReceive(WriteQueue, (void *)&ch, 0) == pdTRUE) {
    ConsolePutChar(ch);
  }
}

static void TtyThread(__unused void *data) {
  /* draw cursor on start */
  ConsoleDrawCursor();

  for (;;) {
    /* wait for notification */
    uint32_t notifyValue = 0;
    xTaskNotifyWait(0, KEY_EVENT | WRITE_EVENT, &notifyValue, portMAX_DELAY);

    /* undraw cursor */
    ConsoleDrawCursor();

    if (notifyValue & KEY_EVENT) {
      /* got some key event */
      KeyEvent_t ev;
      while (xQueueReceive(KeyQueue, (void *)&ev, 0) == pdTRUE)
        handleKeyEvent(ev);

    } else if (notifyValue & WRITE_EVENT) {
      /* got something to write */
      handleWriteEvent();
    }

    /* draw cursor back */
    ConsoleDrawCursor();
  }
}

File_t *TtyOpen(void) {
  static File_t f = {.ops = &TtyOps};
  f.usecount++;
  if (f.usecount == 1) {
    printf("Init tty\n");
    KeyboardInit(PushKeyEventFromISR);

    /* initialize all queues */
    KeyQueue = xQueueCreate(EV_MAXNUM, sizeof(KeyEvent_t));
    ReadQueue = xQueueCreate(MAX_CANON, sizeof(char));
    WriteQueue = xQueueCreate(MAX_WRITE, sizeof(char));

    /* initialize primitive semaphore */
    WriteWaitQueue = xQueueCreate(1, 0);
    /* give first token on queue */
    xQueueSendToBack(WriteWaitQueue, NULL, portMAX_DELAY);

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
    vQueueDelete(KeyQueue);
    vQueueDelete(ReadQueue);
    vQueueDelete(WriteQueue);
    KeyboardKill();
  }
}

static long TtyRead(__unused File_t *f, char *buf, size_t nbyte) {
  if (nbyte == 0)
    return 0;
  char ch;
  buf[0] = '\0';

  /* read from queue until \0 character or reading nbyte characters */
  for (size_t i = 0; i < nbyte; ++i) {
    xQueueReceive(ReadQueue, (void *)&ch, portMAX_DELAY);
    if (ch == '\0') {
      return i;
    }
    buf[i] = ch;
  }

  return nbyte;
}

static long TtyWrite(__unused File_t *f, const char *buf, size_t nbyte) {
  /* take token from Queue */
  xQueueReceive(WriteWaitQueue, NULL, portMAX_DELAY);

  for (size_t i = 0; i < nbyte; ++i) {
    /* send char to queue if can or wait until you can */
    if (xQueueSendToBack(WriteQueue, buf + i, 0) == errQUEUE_FULL) {
      /*
       * there is no space for more characters
       * notify tty driver that queue is full
       * and try again
       */
      i--;
      xTaskNotify(TtyTask, WRITE_EVENT, eSetBits);
    }
  }
  /* to be sure everythig is written */
  xTaskNotify(TtyTask, WRITE_EVENT, eSetBits);

  /* give token on queue */
  xQueueSendToBack(WriteWaitQueue, NULL, portMAX_DELAY);
  return nbyte;
}

static void PushKeyEventFromISR(const KeyEvent_t *ev) {
  /* push key event on queue */
  xQueueSendToBackFromISR(KeyQueue, (const void *)ev, &xNeedRescheduleTask);
  /* notify about key event */
  xTaskNotifyFromISR(TtyTask, KEY_EVENT, eSetBits, &xNeedRescheduleTask);
}
