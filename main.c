/*
    Memoryload simulator
*/



#define _GNU_SOURCE
#include <assert.h>
#include <sched.h> /* getcpu */
#include <stdio.h> 
#include <stdlib.h> 
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>   
#include <pthread.h> 
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <math.h>

#include "topology.h"
#include "workq.h"
#include "ring.h"

//tune
int __g_firstLLC = 0;               // -f first LLCgroup for rings and consumers
                                //1 means skip 0 (use 0 for cli, nic and stats threads)
int __g_ringCnt = 1;                // -r number of rings to build
int __g_llcgroupsPerRing = 1;       // -l number of LLCgroups to share a Ring
//--
int __g_cliAffinity = 1;            // -c cpu to run cli thread on
int __g_ackAffinity = 2;           // -s cpu to run stats gathering thread on
int __g_nicAffinity = 3;            // -n cpu to run nic thread on
int __g_pktLen = 1000;              // -p packet size to replicate
int __g_consumerCnt = 0;            // total accumerlators activated

#define BILLION  1000000000L
#define RINGS_MAX       16          // maximum number of rings on a socket
#define LLCGROUPS_MAX   16          // maximum number of LLCgroups on a socket
#define LLCGROUP_CPU_MAX 16         // maximum number og cpu's per llcGroup
#define PACKET_SIZE_MAX     1800    // maximum packet size that can be generated
#define NUMA_CPU_MAX    256         //TODO limit for now


// commands
#define CMD_CTL_INIT        1
#define CMD_CTL_READY       2
#define CMD_CTL_START       3
#define CMD_CTL_STOP        4
#define CMD_STAT_EVENT      5
#define CMD_EVENT_REQ       10
#define CMD_EVENT_ACK       11


// cli externs
extern int  menuInit();
extern int  menuLoop();
extern int  menuAddItem(char *name, int (*cbFn)(int, char *argv[]) , char *help);
// cli callBacks
int cbGetStats(int argc, char *argv[] );
int cbSaveStats(int argc, char *argv[]);

//
typedef struct ThreadContext_s {
    workq_t *p_workq_in;       // cli control quque
    workq_t *p_ackq_out;    // packet completion ack feedback for nic thread
    int llcGroup;           // info only
    int cpu;                // info only per llc logical cpu number
    int osId;               //OS cpu number used for setting affinity

    int srcId;
    int state;
    char name[32];
    pthread_t   thread_id;

    //stats
    unsigned int errors;
    unsigned int rxCnt;
 
} ThreadContext_t;


typedef struct dir_cpu_entry_s {
    ThreadContext_t context;


}dir_cpu_entry_t;

typedef struct dir_llcGroup_entry_s {
    int cpuCnt;                     //count of cpu's per LLCgroup
    dir_cpu_entry_t cpus[LLCGROUP_CPU_MAX];

} dir_llcGroup_entry_t;

typedef struct dir_ring_entry_s {
    int llcgroupCnt;                // count of active LLCgroups that share this ring
    int cpuCnt;                     // count of cpu's sharing this ring
    dir_llcGroup_entry_t llcGroups[LLCGROUPS_MAX];


} dir_ring_entry_t;


dir_ring_entry_t __g_dir_rings[RINGS_MAX];

//acks per cpu event queues
typedef struct ackq_entry_s {
    struct ackq_entry_s *p_next;
    workq_t workq;
    //stats
    int count;

} ackq_entry_t;

ackq_entry_t __g_ackq[NUMA_CPU_MAX];

typedef struct sendq_entry_s {
    struct sendq_entry_s *p_next;
    workq_t *p_workq;
 
} sendq_entry_t;

sendq_entry_t __g_sendq[NUMA_CPU_MAX];



// contexts for supporting threads
#define CTL_THREAD_CLI      0           // cli thread
#define CTL_THREAD_NIC      1           // packet generator
#define CTL_THREAD_ACKS     2           // feedback thread for NIC, to prevent overrun
#define CTL_THREADS_MAX     3

ThreadContext_t __g_ctlThreads[CTL_THREADS_MAX];

void *th_nic(void *p_arg);
void *th_ack(void *p_arg);
void *th_con(void *p_arg);

/**
 * 
 * 
 * @author martin (11/02/23)
 * @brief start
 * @param argc 
 * @param argv 
 * 
 * @return int 
 */
int main(int argc, char **argv) {
    ThreadContext_t *this = (ThreadContext_t*) &__g_ctlThreads[CTL_THREAD_CLI];
    int i, j, k ,l,m;
    int opt;
    unsigned cpu, numa;
    cpu_set_t my_set;        /* Define your cpu_set bit mask. */
    msg_t                  msg;
    sendq_entry_t *p_sendq = & __g_sendq[0];

 
    topo_init();
    
    menuInit();
    menuAddItem("s", cbGetStats, "get stats");
    menuAddItem("p", cbSaveStats, "save stats to file");

    while((opt = getopt(argc, argv, "hf:r:l:c:n:s:p:")) != -1) 
    { 
        switch(opt) 
        { 
        case 'h':                   //help
            //TODO add usage
            printf("usage\n");
            return 0;
            break;

        case 'f':                    //first ccd
                i = atoi(optarg);
                if (i < topo_getLLCgroupsCnt()) {
                    __g_firstLLC = i;
                }
                else{
                    printf("first LLC must be less than %d\n", topo_getLLCgroupsCnt());
                    return 0;
                }
                break; 


        case 'r':                   // number of rings
                i = atoi(optarg);
                if (i < topo_getLLCgroupsCnt()) {
                    __g_ringCnt = i;
                }
                else{
                    printf("the ring count must be less than %d\n", topo_getLLCgroupsCnt());
                    return 0;
                }
                break; 


       case 'l':                   // number of rllcGroups per ring
                i = atoi(optarg);
                if (i < topo_getLLCgroupsCnt()) {
                    __g_llcgroupsPerRing = i;
                }
                else{
                    printf("the llc/ring must be less than %d\n", topo_getLLCgroupsCnt());
                    return 0;
                }
                break; 


        case 'c':                    //cli cpu mapping
                i = atoi(optarg);
                if(i < (topo_getCpusPerLLCgroup()*topo_getLLCgroupsCnt())) {
                    __g_cliAffinity = i;
                }
                else {
                    printf("cli cpu %d not valid\n", i);
                }
                break; 

       case 's':                    //stats cpu mapping
                i = atoi(optarg);
                if(i < (topo_getCpusPerLLCgroup() * topo_getLLCgroupsCnt())) {
                    __g_ackAffinity = i;
                }
                else {
                    printf("stats cpu %d not valid\n", i);
                }
                break; 

       case 'p':                    //packet size
                i = atoi(optarg);
                if(i < PACKET_SIZE_MAX  && i >= 64) {
                    __g_pktLen = i;
                }
                else {
                    printf("packet size %d not between 64 and %d\n", i, PACKET_SIZE_MAX);
                }
                break; 


        default:
            break;
        }
    }

 
    printf("cli   cpu %d\n", __g_cliAffinity);    
    __g_ctlThreads[CTL_THREAD_CLI].osId = __g_cliAffinity;
    sprintf(__g_ctlThreads[CTL_THREAD_CLI].name, "cli");
    __g_ctlThreads[CTL_THREAD_CLI].p_workq_in = (workq_t *) malloc(sizeof(workq_t));
    workq_init(__g_ctlThreads[CTL_THREAD_CLI].p_workq_in);


    printf("ack cpu %d\n", __g_ackAffinity);    
    __g_ctlThreads[CTL_THREAD_ACKS].osId = __g_ackAffinity;
    sprintf(__g_ctlThreads[CTL_THREAD_ACKS].name, "acks");
    __g_ctlThreads[CTL_THREAD_ACKS].p_workq_in = (workq_t *) malloc(sizeof(workq_t));
    workq_init(__g_ctlThreads[CTL_THREAD_ACKS].p_workq_in);


    printf("nic   cpu %d\n", __g_nicAffinity); 
    __g_ctlThreads[CTL_THREAD_NIC].osId = __g_nicAffinity;
    sprintf(__g_ctlThreads[CTL_THREAD_NIC].name, "nic"); 
    __g_ctlThreads[CTL_THREAD_NIC].p_workq_in = (workq_t *) malloc(sizeof(workq_t));
    workq_init(__g_ctlThreads[CTL_THREAD_NIC].p_workq_in);


    printf("first LLCgroup %d\n", __g_firstLLC);
    printf("LLC total %d - %d = %d that can be used\n", 
            topo_getLLCgroupsCnt(), 
            __g_firstLLC,
            topo_getLLCgroupsCnt() - __g_firstLLC );

    // all command line choices have been made
    // build configuration
    printf("asks:\n");
    printf("    number of rings    %d\n", __g_ringCnt);
    printf("    LLCgroups per ring %d\n", __g_llcgroupsPerRing);
    i = topo_getLLCgroupsCnt() - __g_firstLLC ;
    if(__g_ringCnt*__g_llcgroupsPerRing <= i){
        //proceed
        printf("fits\n");
    }
    else {
        printf("TOO many resources asked for\n");
        return 0;
    }



    getcpu(&cpu, &numa);
    printf("CLI %u %u\n", cpu, numa);

    CPU_ZERO(&my_set); 
    if (__g_cliAffinity >= 0) {
        CPU_SET(__g_cliAffinity, &my_set);
        sched_setaffinity(0, sizeof(cpu_set_t), &my_set);
    }

    getcpu(&cpu, &numa);
    printf("CLI %u %u\n", cpu, numa);
    //workq_init(&g_workq_cli);


    //build directory of resources
    l  = __g_firstLLC;
    m = 0;
    for(i = 0; i < __g_ringCnt; i++){
        __g_dir_rings[i].llcgroupCnt = __g_llcgroupsPerRing;
       __g_dir_rings[i].cpuCnt =  __g_llcgroupsPerRing * topo_getCpusPerLLCgroup();
       for(j = 0; j < __g_llcgroupsPerRing; j++, l++){
            __g_dir_rings[i].llcGroups[j].cpuCnt = topo_getCpusPerLLCgroup();
            for(k= 0; k < __g_dir_rings[i].llcGroups[j].cpuCnt; k++){
                __g_dir_rings[i].llcGroups[j].cpus[k].context.llcGroup = l;
                __g_dir_rings[i].llcGroups[j].cpus[k].context.cpu = k;
                __g_dir_rings[i].llcGroups[j].cpus[k].context.osId = topo_getOsId(0, l,k);
                sprintf(__g_dir_rings[i].llcGroups[j].cpus[k].context.name,
                "con_%02d_%02d_%02d",
                i,l,k);
                __g_dir_rings[i].llcGroups[j].cpus[k].context.p_workq_in = (workq_t *) malloc(sizeof(workq_t));

                workq_init(__g_dir_rings[i].llcGroups[j].cpus[k].context.p_workq_in);
                if(m > 0){
                     __g_ackq[m -1].p_next = &__g_ackq[m];
                     __g_sendq[m-1].p_next = &__g_sendq[m];
                }
                __g_ackq[m].p_next = &__g_ackq[0];
                __g_sendq[m].p_next = NULL;
                __g_sendq[m].p_workq = __g_dir_rings[i].llcGroups[j].cpus[k].context.p_workq_in;
                workq_init(&__g_ackq[m].workq);
                __g_dir_rings[i].llcGroups[j].cpus[k].context.p_ackq_out = &__g_ackq[m].workq;
                 pthread_create(&__g_dir_rings[i].llcGroups[j].cpus[k].context.thread_id, NULL, 
                            th_con, 
                            (void *)  &__g_dir_rings[i].llcGroups[j].cpus[k].context);
                m++;
            }
       }
    }
    __g_consumerCnt = m;
    printf("Total consumers %d\n", m);


    // debug
    /*
    for(i = 0; i < __g_ringCnt; i++){
        printf("Ring[%02d]\n,", i);
         for(j = 0; j < __g_llcgroupsPerRing; j++) {
            printf("\tLLCgroup[%02d]\n",j);
            for(k= 0; k < __g_dir_rings[i].llcGroups[j].cpuCnt; k++){
                printf("\t\t%03d %03d %s\n",
                    k,
                    __g_dir_rings[i].llcGroups[j].cpus[k].context.osId,
                    __g_dir_rings[i].llcGroups[j].cpus[k].context.name);

            }
         }
    }
    */

    //create rings
    //TODO


    //start helper threads
    //ack thread
    pthread_create(&__g_ctlThreads[CTL_THREAD_ACKS].thread_id, NULL, th_ack, (void *) &__g_ctlThreads[CTL_THREAD_ACKS]);
    msg.cmd = CMD_CTL_INIT;
    if(workq_write(__g_ctlThreads[CTL_THREAD_ACKS].p_workq_in, &msg)){
       this->errors++;
       //TODO handle
    }
    //wait for ready
    while(1){
 
        if(workq_read(this->p_workq_in, &msg)){
            if(msg.cmd == CMD_CTL_READY){
                printf("ack thread is ready\n");
                break;
            }
        }
    }


    // nic setup
    pthread_create(&__g_ctlThreads[CTL_THREAD_NIC].thread_id, NULL, th_nic, (void *) &__g_ctlThreads[CTL_THREAD_NIC]);
    msg.cmd = CMD_CTL_INIT;
    if(workq_write(__g_ctlThreads[CTL_THREAD_NIC].p_workq_in, &msg)){
       this->errors++;
       //TODO handle
    }
    //wait for ready
    while(1){
 
        if(workq_read(this->p_workq_in, &msg)){
            if(msg.cmd == CMD_CTL_READY){
                printf("nic thread is ready\n");
                break;
            }
        }
    }

     

    //start consumers
    // send list for now
    while(p_sendq != NULL){
         msg.cmd = CMD_CTL_INIT;
        if(workq_write(p_sendq->p_workq, &msg)){
            this->errors++;
            //TODO handle
        }
        p_sendq = p_sendq->p_next;
    }
    i = 0;
    while(1){
            if(msg.cmd == CMD_CTL_READY){
                printf("ready rx %d (%d)\n",i, __g_consumerCnt);
                i++;
            }
            if( i >= __g_consumerCnt) break;
    }
    printf("%d consumers ready\n", i);


   //start sender
     msg.cmd = CMD_CTL_START;
     msg.length = __g_pktLen;
     msg.src = __g_ringCnt;
     msg.seq = 100;
    if(workq_write(__g_ctlThreads[CTL_THREAD_NIC].p_workq_in, &msg)){
       this->errors++;
       //TODO handle
    }

    
    while (1) {
      if(menuLoop() == 0)  break;
    }

  return 0;
}

int cbSaveStats(int argc, char *argv[]){
    //int n, j, l, c, k, m,sumRx, sumTx;
    FILE *fptr;
    //ThreadContext_t *p_context;

    fptr = fopen("mtest.txt", "w");
    fprintf(fptr, "topology:\n");
    fprintf(fptr,"\tcpu count %d\n", topo_getLLCgroupsCnt() * topo_getCpusPerLLCgroup());
    fprintf(fptr,"\tcli       %d\n", __g_cliAffinity);
    fprintf(fptr,"\tacks      %d\n",  __g_ackAffinity);
    fprintf(fptr,"\tnic       %d\n", __g_nicAffinity);
    fprintf(fptr,"\tconsumers %d\n", __g_consumerCnt);
    fprintf(fptr,"\trings     %d\n", __g_ringCnt);
    fprintf(fptr,"\tllc per ring %d\n", __g_llcgroupsPerRing);
    fprintf(fptr,"\tpkt len   %d\n", __g_pktLen);


 
 

    fclose(fptr);
    return 0;
}

int cbGetStats(int argc, char *argv[] )
{
  
    return 0;
}

/**
 * @author martin (11/02/23) 
 *  
 * @brief acks collecting thread 
 * 
 * @param p_arg  thread context
 * 
 * @return void* 
*/
void *th_ack(void *p_arg){
    ThreadContext_t *this = (ThreadContext_t*) p_arg;
    cpu_set_t           my_set;        /* Define your cpu_set bit mask. */
    msg_t                  msg;
    //int i;
    ackq_entry_t *p_ackq = &__g_ackq[0];
    workq_t *p_workq;
  


    CPU_ZERO(&my_set); 
    if (this->osId >= 0) {
        CPU_SET(this->osId, &my_set);
        sched_setaffinity(0, sizeof(cpu_set_t), &my_set);
    }

    //printf("Thread_%06x PID %d %d cpu %3d %s\n", this->srcId, getpid(), gettid(), this->osId,  this->name);

     while (1) {
         if(workq_read(this->p_workq_in, &msg)){
            if(msg.cmd == CMD_CTL_INIT){
                break;
            }
         }
     }

     //init code here
     


     printf("%s init now\n", this->name);



    msg.cmd = CMD_CTL_READY;
    msg.src = this->srcId;
    msg.length = 0;
    p_workq = (workq_t *) __g_ctlThreads[CTL_THREAD_CLI].p_workq_in;
    if(workq_write(p_workq, &msg)){
        this->errors++;
    }

    
    
    while (1) {

         if(workq_read(&p_ackq->workq, &msg)){
            p_ackq->count++;


         }
        p_ackq = p_ackq->p_next;

        
    }
}





/**
 * @author martin (11/02/23) 
 *  
 * @brief sender  thread 
 * 
 * @param p_arg  thread context
 * 
 * @return void* 
*/
void *th_nic(void *p_arg){
    ThreadContext_t *this = (ThreadContext_t*) p_arg;
    cpu_set_t           my_set;        /* Define your cpu_set bit mask. */
    msg_t                  msg;
    //int i;
    int send_cnt = 10;
    int ringCnt = 1;
    int pktSize = 128;
    int s =1;
    sendq_entry_t *p_sendq = &__g_sendq[0];
    workq_t *p_workq;


    CPU_ZERO(&my_set); 
    if (this->osId >= 0) {
        CPU_SET(this->osId, &my_set);
        sched_setaffinity(0, sizeof(cpu_set_t), &my_set);
    }

    //printf("Thread_%06x PID %d %d cpu %3d %s\n", this->srcId, getpid(), gettid(), this->osId,  this->name);

     while (1) {
         if(workq_read(this->p_workq_in, &msg)){
            if(msg.cmd == CMD_CTL_INIT){
                break;
            }
         }
     }

     //init code here
     


     printf("%s init now\n", this->name);



    msg.cmd = CMD_CTL_READY;
    msg.src = this->srcId;
    msg.length = 0;
    p_workq = __g_ctlThreads[CTL_THREAD_CLI].p_workq_in;
    if(workq_write(p_workq, &msg)){
        this->errors++;
    }

    // wait for start cmd
    while (1) {
        if(workq_read(this->p_workq_in, &msg)){
           if(msg.cmd == CMD_CTL_START){
               send_cnt = msg.seq;
               ringCnt = msg.src;
               pktSize = msg.length;
               printf("%s START send_cnt %d ringCnt %d\n", this->name, send_cnt, ringCnt);
               break;
           }
        }
    }
    
    while (1) {
        if(send_cnt){
            p_sendq = &__g_sendq[0];
            while(p_sendq != NULL){
                msg.cmd = CMD_EVENT_REQ;
                msg.length = pktSize;
                msg.seq = s;
              
                if(workq_write(p_sendq->p_workq, &msg)){
                    this->errors++;
                }
                p_sendq = (sendq_entry_t *) p_sendq->p_next;

            }
            s++;
            send_cnt--;
        }
     
    }
}




/**
 * @author martin (11/02/23) 
 *  
 * @brief consumer  thread 
 * 
 * @param p_arg  thread context
 * 
 * @return void* 
*/
void *th_con(void *p_arg){
    ThreadContext_t *this = (ThreadContext_t*) p_arg;
    cpu_set_t           my_set;        /* Define your cpu_set bit mask. */
    msg_t                  msg;
    //int i;
 

    CPU_ZERO(&my_set); 
    if (this->osId >= 0) {
        CPU_SET(this->osId, &my_set);
        sched_setaffinity(0, sizeof(cpu_set_t), &my_set);
    }

    //printf("Thread_%06x PID %d %d cpu %3d %s\n", this->srcId, getpid(), gettid(), this->osId,  this->name);

     while (1) {
         if(workq_read(this->p_workq_in, &msg)){
            if(msg.cmd == CMD_CTL_INIT){
                break;
            }
         }
     }

     //init code here
     


     printf("%s init now\n", this->name);



    msg.cmd = CMD_CTL_READY;
    msg.src = this->srcId;
    msg.length = 0;
    if(workq_write(__g_ctlThreads[CTL_THREAD_CLI].p_workq_in, &msg)){
        this->errors++;
    }

    
    
    while (1) {
         if(workq_read(this->p_workq_in, &msg)){
            //assume request
            this->rxCnt++;

            msg.cmd = CMD_EVENT_ACK;
            msg.src = this->srcId;
            //msg.length = 0;
            if(workq_write(this->p_ackq_out, &msg)){
                this->errors++;
            }
         }
   
    }
}
