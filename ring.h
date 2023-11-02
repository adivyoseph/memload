#ifndef ___RING_H___
#define ___RING_H___


#typedef struct ringEntry_s {
    struct ringEntry_s *p_next __attribute__ ((aligned(CACHELINE_SIZE)));
    union {
        int seq;
        int length;
    }__attribute__ ((aligned(CACHELINE_SIZE)));
    char data[1800];
} ringEntry_t  __attribute__ ((aligned(CACHELINE_SIZE)));




#endif