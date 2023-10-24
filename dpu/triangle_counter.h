#ifndef __TRIANGLE_COUNTER_H__
#define __TRIANGLE_COUNTER_H__

#include <stdint.h>  //Fixed size integers
#include <stdbool.h>  //Booleans

#include "dpu_util.h"
#include "locate_nodes.h"

//Check if the triplet found is handled by this DPU
bool is_triplet_handled(uint32_t c1, uint32_t c2, uint32_t c3, triplet_t handled_triplet, dpu_arguments_t* DPU_INPUT_ARGUMENTS_PTR);

//from and to are used to divide the workload between tasklets
uint32_t count_triangles(__mram_ptr edge_t* sample, uint32_t edges_in_sample, uint32_t num_locations, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER, void* wram_buffer_ptr);  //to is excluded

//Iterative binary search for finding the informations about a node (possible because the nodes info are ordered)
node_loc_t get_location_info(uint32_t unique_nodes, uint32_t node_id, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER);

#endif /* __TRIANGLE_COUNTER_H__ */
