/*
 * main_periodic_tasks_test.c
 */

#include "main.h"
#include "common.h"
#include "k_task.h"
#include "k_mem.h"
#include <stdio.h>

extern void SystemClock_Config(void);
extern void MX_USART2_UART_Init(void);
//globals
int i_test = 0;
int i_test2 = 0;
void TaskA(void *arg)
{
    (void)arg;
    while (1) {
        printf("%d, %d\r\n", i_test, i_test2);
        osPeriodYield();
    }
}

void TaskB(void *arg)
{
    (void)arg;
    while (1) {
        i_test = i_test + 1;
        osPeriodYield();
    }
}

void TaskC(void *arg)
{
    (void)arg;
    while (1) {
        i_test2 = i_test2 + 1;
        osPeriodYield();
    }
}

// main function
int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_USART2_UART_Init();

    osKernelInit();
    k_mem_init();

    TCB st_mytask;
    st_mytask.stack_size = STACK_SIZE;
    st_mytask.args = NULL;

    /* osCreateDeadlineTask(period_ms, &tcb) */
    st_mytask.ptask = &TaskA;
    osCreateDeadlineTask(4, &st_mytask);

    st_mytask.ptask = &TaskB;
    osCreateDeadlineTask(4, &st_mytask);

    st_mytask.ptask = &TaskC;
    osCreateDeadlineTask(12, &st_mytask);

    osKernelStart();

    while (1) {}
}
