#ifndef __ORDER_AND_LOCATE_H__
#define __ORDER_AND_LOCATE_H__

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

//One partition of the sample where a tasklet will execute QuickSort
typedef struct{
    uint32_t start;
    uint32_t end;
}partition_t;

//Contains all the partitions for the tasklets
typedef struct{
    partition_t* partitions;
    uint32_t size;
}tasklet_partitions_t;

//Using iterative quickSort for ordering in place without using too much stack
void sort_sample(__mram_ptr edge_t* sample, uint32_t start, uint32_t end);
uint32_t partition(__mram_ptr edge_t* array, uint32_t start, uint32_t end);

//Recursive function for creating partitions
void tasklet_partition(__mram_ptr edge_t* sample, uint32_t start, uint32_t end, uint32_t nr_tasklets, tasklet_partitions_t* t_partitions_ptr);

//Debug function used to check if the sample is ordered. end is excluded
bool is_ordered_sample(__mram_ptr edge_t* sample, uint32_t start, uint32_t end);

//Determine the location of each unique node inside the sample, counting also the number of neighbors of that node
//Returns the number of unique nodes
uint32_t node_locations(__mram_ptr edge_t* sample, uint32_t edges_in_sample, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER, void* wram_buffer_ptr);

//Debug function to know the information about the locations of the unique nodes
void print_node_locations(uint32_t number_of_nodes, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER);

#endif /* __ORDER_AND_LOCATE_H__ */
