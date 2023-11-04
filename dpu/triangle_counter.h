#ifndef __TRIANGLE_COUNTER_H__
#define __TRIANGLE_COUNTER_H__

#include <stdint.h>  //Fixed size integers
#include <stdbool.h>  //Booleans

#include "dpu_util.h"
#include "locate_nodes.h"

//from and to are used to divide the workload between tasklets
uint32_t count_triangles(__mram_ptr edge_t* sample, uint32_t edges_in_sample, uint32_t num_locations, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER, void* wram_buffer_ptr);  //to is excluded

//Iterative binary search for finding the informations about a node (possible because the nodes info are ordered)
node_loc_t get_location_info(uint32_t unique_nodes, uint32_t node_id, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER);

#endif /* __TRIANGLE_COUNTER_H__ */
