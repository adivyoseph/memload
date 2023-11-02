#include <stdio.h> 
#include <stdlib.h> 
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>


int __nodeCnt = 0;
int __cpusPerNode = 0;
int __smtOn = 0;
int __llcGroupsPerNode = 0;
int __cpusPerLLCgroup = 0;

#define CPUS_PER_NODE_MAX 512
#define NUMA_NODES_MAX 2 // only support NPS1
#define LLC_PER_NODES_MAX 32 // 
#define CPUS_PER_LLC_MAX 16 // 
typedef struct temp_cpu_s {
    int state;
    int osId;
    int llcGroupId;
} temp_cpu_t ;

 temp_cpu_t  __tempCpus[CPUS_PER_NODE_MAX];

typedef struct cpu_entry_s {
    int active;
    int osId;
} cpu_entry_t;

typedef struct llc_group_s {
    int id;            //system wide
    int cpuCnt;
    cpu_entry_t cpus[CPUS_PER_LLC_MAX];
} llc_group_t;

 typedef struct numa_node_s {
     llc_group_t llc_groups[LLC_PER_NODES_MAX ];
 }  numa_node_t;

  numa_node_t __numaNodes[NUMA_NODES_MAX];

/**
 * 
 * 
 * @author martin (10/15/23) 
 *  
 * @brief walk sysFs and build node0 topology 
 * 
 * @param void 
 * 
 * @return int 
 */
int topo_init(void){
    FILE *p_file;
    DIR *p_folder;
    struct dirent *p_entry;
    char c_work[128];

    int i, j,k,m;

    //determine node count
    // > 2 post a warning and abort
    i = 0;
    p_folder = opendir("/sys/devices/system/node");
    if (p_folder == NULL) {
        return -1;
    }
   while ((p_entry = readdir(p_folder)) != NULL) {
       if(p_entry-> d_type == DT_DIR)  {// if the type is a directory
               //printf("%s\n", p_entry->d_name);
           if (strncmp(p_entry->d_name, "node", 4) == 0) {
                //printf("node %s found\n", p_entry->d_name);
                i++;
            }
       }
    }
   closedir(p_folder);
    //printf("nodes %d\n", i);
    __nodeCnt = i;

    //assume that nodes are identical in size
    // with SMT on node cpu list includes OS ordered cpu list (count)
    //find number of LLCgroups and CPU's per and if SMT is enabled

    i = 0;
    p_folder = opendir("/sys/devices/system/node/node0");
    if (p_folder == NULL) {
        return -1;
    }

    while ((p_entry = readdir(p_folder)) != NULL) {
        //printf("--> %s %d\n", p_entry->d_name, p_entry-> d_type);
        if(p_entry-> d_type == 10)  {// if the type is a directory
            if (strncmp(p_entry->d_name, "cpu", 3) == 0) {
                 //printf("cpu %s found\n", p_entry->d_name);
                 i++;
             }
        }
     }
    closedir(p_folder);
    __cpusPerNode = i;
    printf("cpus per node %d\n", i);

    //build __tempCpus for node0
    for (i = 0; i < __cpusPerNode; i++) {
        //TODO add stat
        sprintf(c_work, "/sys/devices/system/node/node0/cpu%d/cache/index3/id", i);
        //printf("open arg %s\n", c_work);
        p_file = fopen(c_work, "r");
        if (p_file) {
            fgets(c_work, 100, p_file);
           //printf("cpu %2d l3_id %s\n", i , c_work);
            __tempCpus[i].state = 1;
            __tempCpus[i].osId = i;
            __tempCpus[i].llcGroupId = atoi(c_work);

            fclose(p_file);
        }
    }
    //is smt on?
    // /sys/devices/system/node/node0/cpu4/topology/core_cpus_list
    sprintf(c_work, "/sys/devices/system/node/node0/cpu0/topology/core_cpus_list");
    //printf("open arg %s\n", c_work);
    p_file = fopen(c_work, "r");
    if (p_file) {
        fscanf(p_file,"%d,%d", &j, &k);
        //printf("j = %d, k = %d\n", j, k);
        fclose(p_file);
        if (j == 0) {
            if (k > 0) {
                __smtOn = 1;
            }
        }
        if (k == 0) {
            if (j > 0) {
                __smtOn = 1;
            }
        }
    }
    if ( __smtOn > 0) {
        //printf("SMT on\n");
    }

    //find __cpusPerLLCgroup
    j = 0;
    for (i = 0; i < __cpusPerNode; i++) {
        if (__tempCpus[i].llcGroupId == 0) {
            j++;
        }
 
    } 
    __cpusPerLLCgroup = j;
   printf("__cpusPerLLCgroup %d\n", j);


   //llc id may appare in any order, seems to always start with zero
   j = 0;
   for (i = 0; i < __cpusPerNode; i++) {
       if (__tempCpus[i].llcGroupId > j) {
           j = __tempCpus[i].llcGroupId;
       }
   } 
   __llcGroupsPerNode = j+1;
   // printf(" __llcGroupsPerNode %d\n", __llcGroupsPerNode);

   //fill in node structures
   //TODO add support multiple nodes
   printf("===\n");
   if(__smtOn){
        m = __cpusPerLLCgroup/2;
   }
   else {
     m = __cpusPerLLCgroup;
   }

    for (i = 0; i < __cpusPerNode; i++) {
        j = __tempCpus[i].llcGroupId;

        __numaNodes[0].llc_groups[j].id = j;
       k = i;
       while (  k>= m ) {
           k = k - m;
       }
       //account for SMT
       printf("i %03d  llc %02d  k %02d logical ", i, j, k);
       if(__smtOn){
        if(i < __cpusPerNode/2){
            k = k*2;
        }
        else {
            k = k*2 +1;
        }
       }
       printf("%02d\n", k);
       __numaNodes[0].llc_groups[j].cpus[k].osId = i;
 
    }

    //printf("test\n");
/* */
    for (i = 0; i < __llcGroupsPerNode; i++) {
        printf("LLC %d\n", i);
        for (j = 0; j < __cpusPerLLCgroup; j++) {
            printf("\tcore %3d\t cpu 0  OS_id %3d\n", j, __numaNodes[0].llc_groups[i].cpus[j].osId );
 
        }
    }
/* */
    return 0;
}

int topo_getNodeCnt(void){
    return __nodeCnt;
}

int topo_getLLCgroupsCnt(void){
    return __llcGroupsPerNode;
}

int topo_getSMTOn(void){
    return __smtOn;
}

int topo_getCpusPerLLCgroup(void){
    return __cpusPerLLCgroup;
}



int topo_getOsId(int node, int llcgroup, int cpu){
    int osId = -1;
    //TODO add checks
    osId = __numaNodes[node].llc_groups[llcgroup].cpus[cpu].osId;

    return osId;
}
