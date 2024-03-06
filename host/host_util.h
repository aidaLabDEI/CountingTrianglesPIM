#ifndef __HOST_H__
#define __HOST_H__

#include <stdint.h>
#include <sys/time.h>  //Measure execution time

#include "../common/common.h"

/*Wort case, for each edge there is a new unique node.
considering 63MB free in the MRAM for the sample, considering that
for every edge 16 bytes are occupied, there can be 63MB/16B = 4128768 edges
*/
#ifndef MAX_SAMPLE_SIZE
#define MAX_SAMPLE_SIZE 4128768
#endif

#ifndef DPU_BINARY
#define DPU_BINARY "./task"
#endif

typedef struct{
    uint32_t p;
    uint32_t a;
    uint32_t b;
} hash_parameters_t;

//Print how the program should be executed (arguments)
void usage();

//Allocate the DPUs and load the kernel
void* allocate_dpus(void* dpu_set);

//Get time difference between two moments to calculate execution time
float timedifference_msec(struct timeval t0, struct timeval t1);

//Get the number of free bytes in memory
uint64_t get_free_memory();

//Get the parameters for the coloring hash function
hash_parameters_t get_hash_parameters();

//Find t most frequent nodes starting from the data from the threads
uint32_t global_top_freq(node_frequency_t** top_freq_th, node_frequency_t* result_top_f, uint32_t t);

#endif //__HOST_H__
