#ifndef __COMMON_H__
#define __COMMON_H__

#include <stdint.h>

#define EDGES_IN_BATCH 131072  //8byte * 131072 = 1MB

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
