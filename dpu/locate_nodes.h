#ifndef __LOCATE_NODES_H__
#define __LOCATE_NODES_H__

#include <attributes.h>  //For __mram_ptr
#include <stdint.h>   //Fixed size integers

#include "../common/common.h"

//Struct to store informations about one unique node (this speeds up the triangle counting)
typedef struct{
    uint32_t id;
    uint32_t index_in_sample;
    uint32_t number_bigger_neighbors;  //Only the neighbors who have bigger id
    uint32_t padding;  //The struct must be 8 byte aligned for MRAM transfers
} node_loc_t;

//Determine the location of each unique node inside the sample, counting also the number of neighbors of that node
//Returns the number of unique nodes
uint32_t node_locations(__mram_ptr edge_t* sample, uint32_t edges_in_sample, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER, void* wram_buffer_ptr);

//Debug function to know the information about the locations of the unique nodes
void print_node_locations(uint32_t number_of_nodes, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER);

#endif /* __LOCATE_NODES_H__ */
