#ifndef __COMMON_H__
#define __COMMON_H__

#include <stdint.h>

#define EDGES_IN_BATCH 131072  //8byte * 131072 = 1MB

/*There may be two worst cases.
1: Each edge connects two new nodes
2: Each node is connected to the next, creating a "line"
In both cases, for each edge there is a new unique node, so,
considering 60MB free in the MRAM for the sample, considering that
for every edge 24 bytes are occupied, there can be 60MB/24B = 2621440 edges
*/
#define MAX_SAMPLE_SIZE 2621440

//Struct used to transfer starting data from the host to the dpus. Aligned to 8 bytes
typedef struct {
    uint32_t random_seed;

    uint32_t SAMPLE_SIZE;
    uint32_t N_COLORS;

    uint32_t HASH_PARAMETER_P;
    uint32_t HASH_PARAMETER_A;
    uint32_t HASH_PARAMETER_B;
} dpu_arguments_t;

//Contains the two nodes that make an edge
typedef struct {
    uint32_t u;
    uint32_t v;
} edge_t;

//Contains information about the batch
typedef struct {
    uint64_t size;  //Number of valid edges in current batch
    edge_t edges_batch[EDGES_IN_BATCH];
} batch_t;

#endif /* __COMMON_H__ */
