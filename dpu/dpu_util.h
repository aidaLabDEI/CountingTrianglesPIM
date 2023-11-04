#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>  //Fixed size integers
#include <mram.h> //Transfer data between WRAM and MRAM

#include "../common/common.h"

//Size of the heap in the WRAM used as buffer for the MRAM
#ifndef WRAM_BUFFER_SIZE
#define WRAM_BUFFER_SIZE 2048
#endif

//dpu cannot use the standard library random
void srand(uint32_t seed);
uint32_t rand();
uint32_t rand_range(uint32_t from, uint32_t to);  //from and to are included

//Determine the max node id inside the sample to improve quicksort performance
uint32_t determine_max_node_id(__mram_ptr edge_t* sample, uint32_t from_edge, uint32_t to_edge, edge_t* wram_edges_buffer);

//Debug function for printing the content of the sample
void print_sample(__mram_ptr edge_t* sample, uint32_t edges_in_sample);

#endif /* __UTIL_H__ */
