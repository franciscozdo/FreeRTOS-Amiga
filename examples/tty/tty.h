#ifndef _TTY_H_
#define _TTY_H_

#include <FreeRTOS/FreeRTOS.h>
#include <FreeRTOS/task.h>
#include <FreeRTOS/queue.h>
#include <FreeRTOS/semphr.h>

#include <stdio.h>
#include <keyboard.h>
#include <file.h>

File_t *TtyOpen(void);


#endif
