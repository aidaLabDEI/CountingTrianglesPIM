#include <attributes.h>  //For __mram_ptr
#include <stdint.h>  //Fixed size integers
#include <mram.h>  //Transfer data between WRAM and MRAM. Access MRAM
#include <assert.h>  //Assert
#include <stdlib.h>  //Various things
#include <stdio.h>  //Mainly debug messages
#include <stdbool.h>  //Booleans

#include "../common/common.h"
#include "locate_nodes.h"
#include "dpu_util.h"

uint32_t node_locations(__mram_ptr edge_t* sample, uint32_t edges_in_sample, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER, void* wram_buffer_ptr){
    assert(wram_buffer_ptr != NULL);

    uint32_t index = 0;  //Index for traversing the sample

    //Used only the first node of each edge
    uint32_t current_node_id;
    uint32_t index_in_sample;  //Index of the first occurrence of the unique node inside tha sample
    uint32_t number_bigger_neighbors;  //The number of neighbors of the current node. Only the neighbors with bigger ids are considered

    uint32_t mram_heap_offset = 0;  //Offset for the location elements inside the heap, starting from after the space occupied by the sample

    uint32_t unique_nodes = 0;  //Number of unique nodes found

    //Create a buffer in the WRAM of the sample to speed up research
    uint32_t edges_in_wram_cache = (WRAM_BUFFER_SIZE / sizeof(edge_t)) >> 1;  //Divide by 2 with right shift
    //Use half the WRAM buffer for buffering the sample
    edge_t* sample_buffer_wram = (edge_t*) wram_buffer_ptr;

    //Save the nodes info inside the WRAM to make a bigger transfer
    uint32_t nodes_loc_in_wram_cache = (WRAM_BUFFER_SIZE / sizeof(node_loc_t)) >> 1;  //Divide by 2 with right shift
    //Use the other half of the WRAM buffer to store node locations that will be transferred to the MRAM
    node_loc_t* nodes_loc_buffer_wram = (node_loc_t*) wram_buffer_ptr + nodes_loc_in_wram_cache;

    //Indexes to traverse the WRAM buffers
    uint32_t nodes_loc_buffer_index = 0;
    uint32_t sample_buffer_index = edges_in_wram_cache;

    //Search all the unique nodes considering only the first nodes of the edges
    while(index < edges_in_sample){

        //Transfer some edges of the sample to the WRAM
        if(sample_buffer_index == edges_in_wram_cache){
            //If edges_in_sample is not a multiple of edges_in_wram_cache edges, some bytes will be read from MRAM
            //Containing useless informations. This is not a problem because the loop will stop with certain indexes
            read_from_mram(&sample[index], sample_buffer_wram, edges_in_wram_cache * sizeof(edge_t));
            sample_buffer_index = 0;
        }

        //Initialise the variables for this unique node
        number_bigger_neighbors = 0;
        current_node_id = sample_buffer_wram[sample_buffer_index].u;
        index_in_sample = index;
        unique_nodes++;

        //Count the number of nodes that have the current_node as value u in the edge
        //The result is the number of neighbors of that node
        //This is possible because the sample is ordered, there are no duplicates and u<v in each edge
        while(sample_buffer_wram[sample_buffer_index].u == current_node_id){

            number_bigger_neighbors++;
            index++;  //Index in sample
            sample_buffer_index++;

            ///Transfer some edges of the sample to the WRAM
            if(sample_buffer_index == edges_in_wram_cache){
                //If edges_in_sample is not a multiple of edges_in_wram_cache edges, some bytes will be read from MRAM
                //Containing useless informations. This is not a problem because the loop will stop with certain indexes
                read_from_mram(&sample[index], sample_buffer_wram, edges_in_wram_cache * sizeof(edge_t));
                sample_buffer_index = 0;
            }
        }

        //Transfer the data to the mram
        node_loc_t current_node_info = (node_loc_t) {current_node_id, index_in_sample, number_bigger_neighbors, 0};  //0 for padding to 16bytes

        nodes_loc_buffer_wram[nodes_loc_buffer_index] = current_node_info;
        nodes_loc_buffer_index++;

        //Transfer the nodes info to the MRAM if the location buffer is full
        if(nodes_loc_buffer_index == nodes_loc_in_wram_cache){
            write_to_mram(nodes_loc_buffer_wram, AFTER_SAMPLE_HEAP_POINTER + mram_heap_offset, nodes_loc_in_wram_cache * sizeof(node_loc_t));
            mram_heap_offset += nodes_loc_in_wram_cache * sizeof(node_loc_t);
            nodes_loc_buffer_index = 0;
        }
    }

    //Transfer the last node infos
    if(nodes_loc_buffer_index != 0){
        write_to_mram(nodes_loc_buffer_wram, AFTER_SAMPLE_HEAP_POINTER + mram_heap_offset, nodes_loc_in_wram_cache * sizeof(node_loc_t));
    }

    return unique_nodes;
}

//No need to use WRAM buffer because it is only for debugging
void print_node_locations(uint32_t number_of_nodes, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER){
    assert(AFTER_SAMPLE_HEAP_POINTER != NULL);

    printf("Printing the node location informations. There are %d unique nodes:\n", number_of_nodes);

    for(uint32_t i = 0; i < number_of_nodes; i++){
        node_loc_t current_node;
        mram_read(AFTER_SAMPLE_HEAP_POINTER + i * sizeof(node_loc_t), &current_node, sizeof(node_loc_t));  //Read the informations of one node from MRAM
        printf("Id: %d Index in sample: %d Num bigger neighbors: %d\n", current_node.id, current_node.index_in_sample, current_node.number_bigger_neighbors);
    }
}
