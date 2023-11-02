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

//tuune
int __g_firstLLC = 0;               // -f first LLCgroup for rings and consumers
                                //1 means skip 0 (use 0 for cli, nic and stats threads)
int __g_ringCnt = 1;                // -r number of rings to build
int __g_llcgroupsPerRing = 1;       // -l number of LLCgroups to share a Ring
//--
int __g_cliAffinity = 1;            // -c cpu to run cli thread on
int __g_statAffinity = 2;           // -s cpu to run stats gathering thread on
int __g_nicAffinity = 3;            // -n cpu to run nic thread on
int __g_pktLen = 1000;              // -p packet size to replicate

#define BILLION  1000000000L
#define RINGS_MAX       16          // maximum number of rings on a socket
#define LLCGROUPS_MAX   16          // maximum number of LLCgroups on a socket
#define LLCGROUP_CPU_MAX 16         // maximum number og cpu's per llcGroup
#define PACKET_SIZE_MAX     1800    // maximum packet size that can be generated

// cli externs
extern int  menuInit();
extern int  menuLoop();
extern int  menuAddItem(char *name, int (*cbFn)(int, char *argv[]) , char *help);
// cli callBacks
int cbGetStats(int argc, char *argv[] );
int cbSaveStats(int argc, char *argv[]);

//
typedef struct ThreadContext_s {
    workq_t workq_in;
    int llcGroup;
    int cpu;
    int osId;


    int srcId;
    int state;
    char name[32];
    pthread_t   thread_id;

    //stats
    unsigned int errors;
 
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
    int i, j, k ,l;
    int opt;
    unsigned cpu, numa;
    cpu_set_t my_set;        /* Define your cpu_set bit mask. */
 
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
                    __g_statAffinity = i;
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
    printf("stats cpu %d\n", __g_statAffinity);    
    printf("nic   cpu %d\n", __g_nicAffinity); 
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


                 //workq_t workq_in;
 

            }
       }
    }

    // debug
    for(i = 0; i < __g_ringCnt; i++){
        printf("Ring[%02d]\n,", i);
         for(j = 0; j < __g_llcgroupsPerRing; j++) {
            printf("\tLLCgroup[%02d]\n",j);
            for(k= 0; k < __g_dir_rings[i].llcGroups[j].cpuCnt; k++){
                printf("\t\t%02d %02d %s\n",
                    k,
                    __g_dir_rings[i].llcGroups[j].cpus[k].context.osId,
                    __g_dir_rings[i].llcGroups[j].cpus[k].context.name);

            }
         }
    }

  
  while (1) {
      if(menuLoop() == 0)  break;
  }

  return 0;
}

int cbSaveStats(int argc, char *argv[]){
 
    return 0;
}

int cbGetStats(int argc, char *argv[] )
{
  
    return 0;
}

