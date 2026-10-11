#ifndef INC_COMMON_H_
#define INC_COMMON_H_

#include <stdint.h>

/* Task IDs / limits */
#define TID_NULL   0
#define MAX_TASKS  16
#define STACK_SIZE 0x200

/* Task states */
#define DORMANT   0
#define READY     1
#define RUNNING   2
#define SLEEPING  3   

/* Return codes */
#define RTX_OK    0
#define RTX_ERR  (-1)

/* Types */
typedef unsigned int   U32;
typedef unsigned short U16;
typedef char           U8;
typedef unsigned int   task_t;

/* SVC numbers*/
#define SVC_KINIT       0
#define SVC_CREATE      1
#define SVC_START       2
#define SVC_YIELD       3
#define SVC_TASKINFO    4
#define SVC_GETTID      5
#define SVC_EXIT        6

/* SVC numbers*/
#define SVC_MEM_INIT    7
#define SVC_MEM_ALLOC   8
#define SVC_MEM_DEALLOC 9
#define SVC_MEM_EXTFRAG 10

/* SVC numbers*/
#define SVC_SLEEP           11
#define SVC_PERIOD_YIELD    12
#define SVC_SET_DEADLINE    13
#define SVC_CREATE_DEADLINE 14

#define SVC_CALL(NUM) __asm volatile ("svc %0" : : "I"(NUM) : "memory")

#endif /* INC_COMMON_H_ */
