#ifndef __WORKQ_H__
#define __WORKQ_H__

/*
 workq used for simple simulator IPC
 
*/


#ifndef CACHELINE_SIZE
#define CACHELINE_SIZE 64
#endif

#define FIFO_DEPTH_MAX 0xff

#define USE_LOCK 1   //use pthread_spinlock, its about three times faster than mutex

typedef struct msg_s {
    int cmd;
    int src;
    int data;
    int length;
    struct timespec start;
} msg_t;


typedef struct {
#ifdef USE_LOCK
   pthread_spinlock_t lock  __attribute__ ((aligned(CACHELINE_SIZE)));
  #else
  pthread_mutex_t lock __attribute__ ((aligned(CACHELINE_SIZE)));
 #endif
    volatile int head ; //__attribute__ ((aligned(CACHELINE_SIZE)));
    volatile int tail; // __attribute__ ((aligned(CACHELINE_SIZE)));
    msg_t event[FIFO_DEPTH_MAX] __attribute__ ((aligned(CACHELINE_SIZE)));
} workq_t __attribute__ ((aligned(CACHELINE_SIZE)));



extern int      workq_init(workq_t *p_q);
extern int      workq_write(workq_t *p_q, msg_t  *p_msg);
extern int      workq_read(workq_t *p_q, msg_t  *p_msg);


#endif
