#ifndef ___TOPOLOGY_H___
#define ___TOPOLOGY_H___


extern int topo_init(void);
extern int topo_getNodeCnt(void);
extern int topo_getLLCgroupsCnt(void);
extern int topo_getSMTOn(void);
extern int topo_getCoresPerLLCgroup(void);
extern int topo_getCpusPerLLCgroup(void);
extern int topo_getOsId(int node, int llcgroup, int core, int cpu);

#endif