/*
    Actor simulator
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

//tune

#define SEND_CREDITS_MAX 4
#define BILLION  1000000000L


extern int  menuInit();
extern int  menuLoop();
extern int  menuAddItem(char *name, int (*cbFn)(int, char *argv[]) , char *help);

int cbGetStats(int argc, char *argv[] );
int cbSaveStats(int argc, char *argv[]);

#define STATS_SAMPLES_MAX 16
typedef struct stats_entry_s {
    long count;
    int  samples[STATS_SAMPLES_MAX];
}stats_entry_t;

int firstLLC = 0;
int offset_llcCpuSet;
int stats_totalEntries;
long stats_sampleBase = 0.1;
long stats_sampleSlot = 0.1;


//
typedef struct actorThreadContext_s {
    workq_t workq_in;
    int node;
    int llcGroup;
    int core;
    int cpu;
    int osId;


    int srcId;
    int state;
    char name[32];
    pthread_t   thread_id;

    //stats
    unsigned int errors;
    stats_entry_t *p_statsTx;
    stats_entry_t *p_statsRx;

} actorThreadContext_t;


// directory services
typedef struct dir_cpu_s {
    int state;
    actorThreadContext_t context;
} dir_cpu_t;
#define DIR_CPUCNT_MAX 32

typedef struct dir_llcGroup_s {
    int coreCnt;
    int cpuCnt;
    dir_cpu_t cpus[DIR_CPUCNT_MAX];
} dir_llcGroup_t;
#define DIR_LLCGROUP_MAX 16

typedef struct dir_node_s {
    int llcGroupCnt;
    dir_llcGroup_t llcGroups[DIR_LLCGROUP_MAX];
} dir_node_t;
dir_node_t __nodes[2];

void dir_init(void){
    int i,j,k, l;
    actorThreadContext_t *p_context;

    __nodes[0].llcGroupCnt = topo_getLLCgroupsCnt();
    for (i = firstLLC; i < __nodes[0].llcGroupCnt; i++) {
        __nodes[0].llcGroups[i].coreCnt = topo_getCoresPerLLCgroup();
        __nodes[0].llcGroups[i].cpuCnt = __nodes[0].llcGroups[i].coreCnt * (1 + topo_getSMTOn());
        for (j = 0; j < __nodes[0].llcGroups[i].cpuCnt; j++) {
            __nodes[0].llcGroups[i].cpus[j].state = 0;
            p_context = &__nodes[0].llcGroups[i].cpus[j].context;
            p_context->node = 0;
            p_context->llcGroup = i;
            p_context->p_statsTx = (stats_entry_t *)malloc(stats_totalEntries *sizeof(stats_entry_t));
            for (k = 0; k < stats_totalEntries; k++) {
                p_context->p_statsTx[k].count = 0;

            }
            p_context->p_statsRx = (stats_entry_t *)malloc(stats_totalEntries*sizeof(stats_entry_t));
            for (k = 0; k < stats_totalEntries; k++) {
                p_context->p_statsRx[k].count = 0;
                for (l = 0; l < 16; l++) {
                    p_context->p_statsRx[k].samples[l] = 0;

                }
            }
            if (topo_getSMTOn() > 0) {
                p_context->core = j /2;
                p_context->cpu = j & 0x001;
                p_context->osId = topo_getOsId(0, i,p_context->core,p_context->cpu);
                p_context->srcId = (p_context->node << 16) | (i << 8) | j;
                //printf("srcId %x\n", p_context->srcId);
            }
            else{
                p_context->core = j;
                p_context->cpu = 0;
                p_context->osId = topo_getOsId(0, i,j,0);

                p_context->srcId = (p_context->node << 16) | (i << 8) | j;
            }

        }
    }
}

actorThreadContext_t *dir_getContext(int node, int llcgroup, int cpu){
    actorThreadContext_t *p_context = NULL;
    if (node == 0) {
        if (llcgroup < __nodes[node].llcGroupCnt) {
            if (cpu < __nodes[node].llcGroups[llcgroup].cpuCnt) {
               if (__nodes[node].llcGroups[llcgroup].cpus[cpu].state > 0) {
                   p_context = &__nodes[node].llcGroups[llcgroup].cpus[cpu].context;
               }
            }
        }
    }
    return p_context;
}


actorThreadContext_t *dir_setContext(int node, int llcgroup){
    actorThreadContext_t *p_context = NULL;
     int i;
    if (node == 0) {
        if (llcgroup < __nodes[node].llcGroupCnt) {
            // find next avalable cpu
            for (i = 0; i < __nodes[node].llcGroups[llcgroup].cpuCnt; i++) {
                if (__nodes[node].llcGroups[llcgroup].cpus[i].state == 0) {
                    __nodes[node].llcGroups[llcgroup].cpus[i].state = 1;
                    p_context = &__nodes[node].llcGroups[llcgroup].cpus[i].context;
                    break;
                }
            }
        }
    }
    return p_context;
}



actorThreadContext_t *dir_findContext(int node, int llcgroup, int cpu, char *p_name){
    actorThreadContext_t *p_context = NULL;
    if (node == 0) {
        if (llcgroup < __nodes[node].llcGroupCnt) {
            if (cpu < __nodes[node].llcGroups[llcgroup].cpuCnt) {
               if (__nodes[node].llcGroups[llcgroup].cpus[cpu].state > 0) {
                   p_context = &__nodes[node].llcGroups[llcgroup].cpus[cpu].context;
                    if (strncmp(p_name, p_context->name, 4) == 0) {
                        //found one
                    }
                    else {
                        p_context = NULL;
                    }
                }
               
            }
        }
    }
    return p_context;
}


void stats_init(void){
    int i;

    for (i = 1; i < 8; i++) {
        if ((topo_getCpusPerLLCgroup() <= (1 << i)) &&
            (topo_getCpusPerLLCgroup() > (1 << (i -1)))){
            offset_llcCpuSet = (1 << i);
            break;
        }
    }
    stats_totalEntries = topo_getLLCgroupsCnt()*offset_llcCpuSet;
}

#define STATS_INDEX(a)  (( a >> 8)*offset_llcCpuSet)+(a & 0x0ff)

void *th_send(void *p_arg);
void *th_recv(void *p_arg);

// commands
#define CMD_CTL_INIT        1
#define CMD_CTL_READY       2
#define CMD_CTL_START       3
#define CMD_CTL_STOP        4
#define CMD_CTL_CLEAR       5
#define CMD_REQ_LAST        8
#define CMD_REQ             9
#define CMD_REQ_ACK        10

// queue for CLI control thread
workq_t g_workq_cli;

/**
 * 
 * 
 * @author martin (10/15/23)
 * @brief start
 * @param argc 
 * @param argv 
 * 
 * @return int 
 */
int main(int argc, char **argv) {
    int actorsPerLLCgroup = 0;
    int i, j, k, l, m;
    actorThreadContext_t *p_context;
    msg_t                  msg;
    int totalReqs = 1000000;  // 1M
    int numSenders = -1;
    int numRecrs = -1;

    cpu_set_t my_set;        /* Define your cpu_set bit mask. */
    int cliAffinity = 1;

    unsigned cpu, numa;
    int opt;


    struct timespec start;
    struct timespec end;
    double accum;

    topo_init();
    stats_init();
    menuInit();
    menuAddItem("s", cbGetStats, "get stats");
    menuAddItem("p", cbSaveStats, "save stats to file");

    while((opt = getopt(argc, argv, "hc:s:r:f:")) != -1) 
    { 
        switch(opt) 
        { 
        case 'h':                   //help
            printf("usage\n");
            return 0;
            break;

        case 'f':                    //first ccd
                i = atoi(optarg);
                if (i < topo_getLLCgroupsCnt()) {
                    firstLLC = i;
                }
                else{
                    printf("first LLC must be less tha %d\n", topo_getLLCgroupsCnt());
                    return 0;
                }
                break; 


        case 'c':                    //cli cpu mapping
                cliAffinity = atoi(optarg);
                printf("cli cpu %d\n", cliAffinity); 
                break; 

        case 's':                    //ag cpu mapping
                i = atoi(optarg);
                numSenders = i;
                break; 


        case 'r':                    //ag cpu mapping
                i = atoi(optarg);
                numRecrs = i;
                break; 
        default:
            break;
        }
    }

    if (numSenders < 0) {
        numSenders = 4;
        printf("number of senders %d (default)\n", numSenders);
    }
    j = ((topo_getCpusPerLLCgroup() -1)/2)*(topo_getLLCgroupsCnt() - firstLLC);
    if (numSenders <= j) {
        printf("number of senders %d\n", numSenders);
    }
    else {
        printf("On this system max senders is %d (%d not valid)\n", j, numSenders);
        return 0;
    }

    if (numRecrs < 0) {
        numRecrs = numSenders;
        printf("number of receivers %d (default)\n", numRecrs);
    }

    j = ((topo_getCpusPerLLCgroup() -1)/2)*(topo_getLLCgroupsCnt() - firstLLC);
    if (numRecrs <= j) {

        printf("number of receivers %d\n", numRecrs);
    }
    else {
        printf("On this system max receivers is %d (%d not valid)\n", j, numRecrs);
        return 0;
    }

    printf("first LLCgroup %d\n", firstLLC);


    getcpu(&cpu, &numa);
    printf("CLI %u %u\n", cpu, numa);

    CPU_ZERO(&my_set); 
    if (cliAffinity >= 0) {
        CPU_SET(cliAffinity, &my_set);
        sched_setaffinity(0, sizeof(cpu_set_t), &my_set);
    }

    getcpu(&cpu, &numa);
    printf("CLI %u %u\n", cpu, numa);
    workq_init(&g_workq_cli);

    if (topo_getNodeCnt() != 1) {
        printf("Error: only single NUMA node supported for now\n");
        exit(1);
    }
    // assume single node for now
    dir_init();
    actorsPerLLCgroup = (topo_getCpusPerLLCgroup() -1)/2;
    printf("actorsPerLLCgroup %d\n", actorsPerLLCgroup);

    for (i = firstLLC; i < topo_getLLCgroupsCnt(); i++) {
        for (j = 0; j < actorsPerLLCgroup; j++ ){
            //create senders
            p_context = dir_setContext(0, i);
            if (p_context != NULL) {
                sprintf(p_context->name, "send_%d_%d", i, j);
                workq_init(&p_context->workq_in);
                pthread_create(&p_context->thread_id, NULL, th_send, (void *) p_context);
                //printf("context set llc %2d send %2d %s\n", i, j, p_context->name);

            }
        }

        for (j = 0; j < actorsPerLLCgroup; j++ ){
            //create receivers
            p_context = dir_setContext(0, i );
            if (p_context != NULL) {
                sprintf(p_context->name, "recv_%d_%d", i, j);
                workq_init(&p_context->workq_in);
                pthread_create(&p_context->thread_id, NULL, th_recv, (void *) p_context);
                //printf("context set llc %2d recv %2d %s\n", i, j, p_context->name);
            }
        }
    }
/* 
    for (i = 0, k = 0; i < topo_getLLCgroupsCnt(); i++) {
        for (j = 0; j < topo_getCpusPerLLCgroup(); j++) {
            printf("llc %2d dir_cpu %2d state %d  os_cpu %3d\n",
                   i, j,  __nodes[0].llcGroups[i].cpus[j].state, __nodes[0].llcGroups[i].cpus[j].context.osId);
        }
    }
 */
  // send init as all threads registered
  for (i = firstLLC, k = 0; i < topo_getLLCgroupsCnt(); i++) {
      for (j = 0; j < topo_getCpusPerLLCgroup(); j++) {
          p_context = dir_getContext(0, i, j);
          if (p_context != NULL) {
              msg.cmd = CMD_CTL_INIT;
              msg.src = -1;
              msg.length = 0;
              if(workq_write(&p_context->workq_in, &msg)){
                  printf("%d q is full\n", p_context->srcId);
              }
              else{
                  k++;
              }
          }
      }
  }

  //wait for initialization to complete on all threads
  i = 0;
  while (1) {
         if(workq_read(&g_workq_cli, &msg)){
             if(msg.cmd == CMD_CTL_READY) {
                 i++;
                 //printf("thread %d is ready\n", msg.src);
             }
          if (i  >= k) {
              break;
          }
      }
  }
  printf("All threads ready\n");

  //send start

  l = totalReqs/numSenders;
  m = totalReqs - (l *(numSenders -1));  //first sender difference

  printf("reg send %d first send %d\n", l,m);

  clock_gettime(CLOCK_REALTIME, &start);
  for (i = firstLLC, k = 0; i < topo_getLLCgroupsCnt(); i++) {
      if (k >= numSenders) {
          break;
      }
      for (j = 0; j < topo_getCpusPerLLCgroup(); j++) {
          p_context = dir_findContext(0, i, j, "send");
          if (p_context != NULL) {
              msg.cmd = CMD_CTL_START;
              msg.src = -1;
              if (k == 0){
                  msg.length = m;
              }
              else {
                  msg.length = l;
              }
              msg.data = numRecrs;
              if(workq_write(&p_context->workq_in, &msg)){
                  printf("%d q is full\n", p_context->srcId);
              }
              k++;
              if (k >= numSenders) {
                  break;
              }
          }
      }
  }

  i = 0;
  while (1) {
      //wait for STOP
      if(workq_read(&g_workq_cli, &msg)){
           //printf("stop from %d \n", msg.src);
          if(msg.cmd == CMD_CTL_STOP) {
              i++;
              if (i == k) {
                  clock_gettime(CLOCK_REALTIME, &end);
                  break;
              }
          }
      }
     // menuLoop();
  }
  printf("finished\n");

  accum = ( end.tv_sec - start.tv_sec ) + (double)( end.tv_nsec - start.tv_nsec ) / (double)BILLION;
  printf( "%lf\n", accum );

  while (1) {
      if(menuLoop() == 0)  break;
  }

  return 0;
}

int cbSaveStats(int argc, char *argv[]){
    int n, j, l, c, k, m,sumRx, sumTx;
    FILE *fptr;
    actorThreadContext_t *p_context;

    fptr = fopen("topology.txt", "w");
    fprintf(fptr, "topology\n");
    fprintf(fptr, "Node,LLCgroup,CPU, OSid, App, state\n");
    for (n = 0; n < topo_getNodeCnt(); n++) {
        for (l = firstLLC; l < topo_getLLCgroupsCnt(); l++ ) {
            for (c = 0; c < topo_getCpusPerLLCgroup(); c++) {
                fprintf(fptr, "%2d,%2d,%2d,%3d,%s,",
                        n,l,c,
                        __nodes[n].llcGroups[l].cpus[c].context.osId,
                        __nodes[n].llcGroups[l].cpus[c].context.name);
                if (__nodes[n].llcGroups[l].cpus[c].state == 1) {
                    fprintf(fptr,"E\n");
                }
                else{
                    fprintf(fptr,".\n");
                }
            }
        }
    }
    fclose(fptr); 
    fptr = fopen("send.txt", "w");
    for (l = firstLLC; l < topo_getLLCgroupsCnt(); l++) {
        for (c = 0; c< topo_getCpusPerLLCgroup(); c++) {
            p_context = dir_findContext(0, l, c, "send");
            if (p_context != NULL) {
                sumTx = 0;
                sumRx = 0;
                for (k = 0; k < stats_totalEntries; k++) {
                    sumTx += p_context->p_statsTx[k].count;
                    sumRx += p_context->p_statsRx[k].count;
                }
                if (sumTx == 0 && sumRx == 0) {
                     continue;
                }
                fprintf(fptr, "%02d,%02d,", l, c);
                fprintf(fptr,"%6d,%6d, %4d\n", sumTx, sumRx ,p_context->errors);

                for (k = firstLLC; k < topo_getLLCgroupsCnt(); k++) {
                    for (m = 0; m < topo_getCpusPerLLCgroup(); m++) {
                        if (p_context->p_statsTx[k*offset_llcCpuSet + m].count == 0) {
                           continue;
                        }
                        fprintf(fptr, "%2d,%2d,%6ld,%6ld,--",
                                k, m, p_context->p_statsTx[k*offset_llcCpuSet + m].count, p_context->p_statsRx[k*offset_llcCpuSet + m].count);
                        for (j = 0; j < 16; j++) {
                            fprintf(fptr, ",%6d",p_context->p_statsRx[k*offset_llcCpuSet + m].samples[j]);
                        }
                        fprintf(fptr, "\n");
                    }
                }
            }
        }
    }
    fclose(fptr); 
    return 0;
}

int cbGetStats(int argc, char *argv[] )
{
    int i, j, k, l, sumRx, sumTx;
    actorThreadContext_t *p_context;

    printf("Mapping\n");
    for (i = firstLLC; i < topo_getLLCgroupsCnt(); i++) {
        printf("llc_%02d  ",i);
        for (j = 0; j < topo_getCpusPerLLCgroup(); j++) {
            printf("\t%02d", j);

        }
        printf("\n        ");
        for (j = 0; j < topo_getCpusPerLLCgroup(); j++) {
            printf("\t%02d", __nodes[0].llcGroups[i].cpus[j].context.osId);
        }

        printf("\n        ");
        for (j = 0; j < topo_getCpusPerLLCgroup(); j++) {
            if (__nodes[0].llcGroups[i].cpus[j].state > 0) {
                printf("\t%.*s ", 1, __nodes[0].llcGroups[i].cpus[j].context.name);
            }
            else{
                printf("\t--");
            }
        }
        printf("\n");
    }
    printf("\nSend:\n");
    for (i = firstLLC; i < topo_getLLCgroupsCnt(); i++) {
        for (j = 0; j < topo_getCpusPerLLCgroup(); j++) {
            p_context = dir_findContext(0, i, j, "send");
            if (p_context != NULL) {
                printf("llc %02d cpu %02d  ",i,j);
                sumTx = 0;
                sumRx = 0;
                for (k = 0; k < stats_totalEntries; k++) {
                    sumTx += p_context->p_statsTx[k].count;
                    sumRx += p_context->p_statsRx[k].count;
                }
                printf("%6d   %6d\n", sumTx, sumRx);

                for (k = 0; k < stats_totalEntries; k++) {
                     printf("           %02d %6ld   %6ld   --  ",k, p_context->p_statsTx[k].count, p_context->p_statsRx[k].count);
                    for (l = 0; l <16; l++) {
                        printf("\t%3d",p_context->p_statsRx[k].samples[l]);
                    }
                    printf("\n");
                }
            }
        }
    }
    printf("\nRecv:\n");
    for (i = firstLLC; i < topo_getLLCgroupsCnt(); i++) {
        for (j = 0; j < topo_getCpusPerLLCgroup(); j++) {
            p_context = dir_findContext(0, i, j, "recv");
            if (p_context != NULL) {
                printf("llc %02d cpu %02d  ",i,j);
                sumTx = 0;
                sumRx = 0;
                for (k = 0; k < stats_totalEntries; k++) {
                    sumTx += p_context->p_statsTx[k].count;
                    sumRx += p_context->p_statsRx[k].count;
                }
                printf("%6d   %6d\n", sumTx, sumRx);
            }
        }
    }
    return 0;
}

typedef struct th_dest_entry_s{
    int destId;
    int credits;
    workq_t *p_workq;
} th_dest_entry_t;

/**
 * 
 * 
 * @author martin (10/25/23) 
 *  
 * @brief request send thread 
 * 
 * @param p_arg  thread context
 * 
 * @return void* 
 */
void *th_send(void *p_arg){
    actorThreadContext_t *this = (actorThreadContext_t*) p_arg;
    cpu_set_t           my_set;        /* Define your cpu_set bit mask. */
     msg_t                  msg;
     int i, j, k;
     int th_destCnt = 0;
     th_dest_entry_t *p_th_destTable;
     actorThreadContext_t *p_context;
     int send_cnt = 0;
     int send_destCnt = 0;
     //workq_t *p_workq;
    struct timespec end;
    double accum;
    int debug = 0;


    CPU_ZERO(&my_set); 
    if (this->osId >= 0) {
        CPU_SET(this->osId, &my_set);
        sched_setaffinity(0, sizeof(cpu_set_t), &my_set);
    }

    //printf("Thread_%06x PID %d %d cpu %3d %s\n", this->srcId, getpid(), gettid(), this->osId,  this->name);

     while (1) {
         if(workq_read(&this->workq_in, &msg)){
            if(msg.cmd == CMD_CTL_INIT){
                break;
            }
         }
     }

     //init code here
     i = topo_getLLCgroupsCnt() * topo_getCpusPerLLCgroup();

     p_th_destTable = malloc(i * sizeof(th_dest_entry_t));
     //populate table
     for (i = firstLLC, k = 0; i < topo_getLLCgroupsCnt(); i++) {
         for (j = 0; j < topo_getCpusPerLLCgroup(); j++) {
             p_context = dir_findContext(0, i, j, "recv");
             if (p_context != NULL) {
                 p_th_destTable[k].credits = SEND_CREDITS_MAX;  //TODO change
                 p_th_destTable[k].destId = p_context->srcId;
                 p_th_destTable[k].p_workq = &p_context->workq_in;
                 k++;
             }
         }
     }
     th_destCnt = k;


     //printf("%s%d init now\n", this->name, this->instance);



    msg.cmd = CMD_CTL_READY;
    msg.src = this->srcId;
    msg.length = 0;
    if(workq_write(&g_workq_cli, &msg)){
        printf("%d q is full\n", this->srcId);
    }

    // wait for start cmd
    while (1) {
        if(workq_read(&this->workq_in, &msg)){
           if(msg.cmd == CMD_CTL_START){
               send_cnt = msg.length;
               send_destCnt = msg.data;
               //printf("%s START send_cnt %d destCnt %d\n", this->name, send_cnt, send_destCnt);
               break;
           }
        }
    }
    j = 0;
    while (1) {
        //look for acks
        if(workq_read(&this->workq_in, &msg)){
            clock_gettime(CLOCK_REALTIME, &end);
            accum = ( end.tv_sec - msg.start.tv_sec ) + (double)( end.tv_nsec - msg.start.tv_nsec ) / (double)BILLION;
            if (debug > 0) {
                printf("rtt accum %lf\n", accum );
            }
            accum = trunc(accum / 0.00001);
            if(debug > 0) {
                printf("rtt accuy %lf\n", accum );
            }
            i= (int)accum;
            if (i > 15) {
                i = 15;
            }

            if (debug > 0) {
                //printf("rtt accuy %d\n", i );
            }
            this->p_statsRx[STATS_INDEX(msg.src)].samples[i]++;
            debug--;
            this->p_statsRx[STATS_INDEX(msg.src)].count++;
            //assume ack for now
            for(i = 0; i < th_destCnt; i++){
                if (msg.src == p_th_destTable[i].destId) {
                    p_th_destTable[i].credits++;
                    break;
                }
            }
        }
        //now try to send something
        if (send_cnt > 0) {
            if (p_th_destTable[j].credits > 0) {
                //sent a request
                msg.cmd = CMD_REQ;
                msg.src = this->srcId;
                msg.length = send_cnt;
                clock_gettime(CLOCK_REALTIME, &msg.start);
                if(workq_write(p_th_destTable[j].p_workq, &msg)){
                    this->errors++;
                }
                else {
                    //printf("%s sent REG to %08x %d\n", this->name, p_th_destTable[j].destId, send_cnt);
                    this->p_statsTx[STATS_INDEX(p_th_destTable[j].destId)].count++;
                    p_th_destTable[j].credits--;
                    send_cnt --;
                }
                j++;
                if (j >= send_destCnt) {
                    j = 0;
                }
            }
        }
        else {
            //check for outstanding acks

            for(i = 0; i < th_destCnt; i++){
                if(p_th_destTable[i].credits < SEND_CREDITS_MAX) break;
            }
            if (i >= th_destCnt) {
                msg.cmd = CMD_CTL_STOP  ;
                msg.src = this->srcId;
                msg.length = 0;
                if(workq_write(&g_workq_cli, &msg)){
                    this->errors++;
                }
                else{
                    //printf("%s sent stop\n", this->name);
                    break;
                }
            }
        }
    }
    free(p_th_destTable);

    while (1){
    }

}

/**
 * 
 * 
 * @author martin (10/25/23) 
 *  
 * @brief receive thread 
 * 
 * @param p_arg 
 * 
 * @return void* 
 */
void *th_recv(void *p_arg){
    actorThreadContext_t *this = (actorThreadContext_t*) p_arg;
    cpu_set_t           my_set;        /* Define your cpu_set bit mask. */
     msg_t                  msg;
     int i, j, k;
     int th_destCnt = 0;
     th_dest_entry_t *p_th_destTable;
     actorThreadContext_t *p_context;
     workq_t *p_workq;


    CPU_ZERO(&my_set); 
    if (this->osId >= 0) {
        CPU_SET(this->osId, &my_set);
        sched_setaffinity(0, sizeof(cpu_set_t), &my_set);
    }

    //printf("Thread_%06x PID %d %d cpu %3d %s\n", this->srcId, getpid(), gettid(), this->osId, this->name);

     while (1) {
         if(workq_read(&this->workq_in, &msg)){
            if(msg.cmd == CMD_CTL_INIT){
                break;
            }
         }
     }

     //init code here
     i = topo_getLLCgroupsCnt() * topo_getCpusPerLLCgroup();

     p_th_destTable = malloc(i * sizeof(th_dest_entry_t));
     //populate table
     for (i = firstLLC, k = 0; i < topo_getLLCgroupsCnt(); i++) {
         for (j = 0; j < topo_getCpusPerLLCgroup(); j++) {
             p_context = dir_findContext(0, i, j, "send");
             if (p_context != NULL) {
                 //p_th_destTable[k]->credits = 4;
                 p_th_destTable[k].destId = p_context->srcId;
                 p_th_destTable[k].p_workq = &p_context->workq_in;
                 //printf("%s dest %2d %08x\n", this->name, k, p_th_destTable[k].destId);
                 k++;
             }
         }
     }
     th_destCnt = k;
     //printf("%s%d init now\n", this->name, this->instance);



    msg.cmd = CMD_CTL_READY;
    msg.src = this->srcId;
    msg.length = 0;
    if(workq_write(&g_workq_cli, &msg)){
        printf("%d q is full\n", this->srcId);
    }

    //ok to drop into running mode

    while (1){
        if(workq_read(&this->workq_in, &msg)){
            this->p_statsRx[STATS_INDEX(msg.src)].count++;
            //printf("%s %08x RECV from %08x seq %d\n", this->name, this->srcId, msg.src, msg.length);
            //assume request for now
            for(i = 0; i < th_destCnt; i++){
                if (msg.src == p_th_destTable[i].destId) {
                    p_workq = p_th_destTable[i].p_workq;
                    msg.src = this->srcId;
                    msg.cmd = CMD_REQ_ACK;

                    if (workq_write(p_workq, &msg)) {
                        //printf("%d q is full\n", this->srcId);
                        this->errors++;
                    }
                    else {
                        this->p_statsTx[STATS_INDEX(p_th_destTable[i].destId)].count++;
                        //printf("%s %08x send ack to %08x seq %d\n", this->name, this->srcId, p_th_destTable[i].destId, msg.length);
                    }
                    break;
                }
            }
        }
    }

    free(p_th_destTable);
}


