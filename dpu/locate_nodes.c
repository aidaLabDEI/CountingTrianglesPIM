#include <attributes.h>  //For __mram_ptr
#include <stdint.h>  //Fixed size integers
#include <mram.h>  //Transfer data between WRAM and MRAM. Access MRAM
#include <stdlib.h>  //Various things
#include <stdio.h>  //Mainly debug messages
#include <stdbool.h>  //Booleans
#include <defs.h>  //Get tasklet id
#include <handshake.h>  //Handshake for tasklets

#include "../common/common.h"
#include "locate_nodes.h"
#include "dpu_util.h"

//Offset where to write and read in the MRAM common for all tasklets
uint32_t global_read_offset = 0;
uint32_t global_write_offset = 0;

uint32_t node_locations(__mram_ptr edge_t* sample, uint32_t edges_in_sample, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER, void* wram_buffer_ptr){
    //Create a buffer in the WRAM of the sample to speed up research
    uint32_t max_edges_in_wram_cache = (WRAM_BUFFER_SIZE / sizeof(edge_t)) >> 1;  //Divide by 2 with right shift
    //Use half the WRAM buffer for buffering the sample
    edge_t* sample_buffer_wram = (edge_t*) wram_buffer_ptr;

    //Use the other half of the WRAM buffer to store node locations that will be transferred to the MRAM
    //Because the size of an edge variable is the same as the size of a node location variable,
    //There is no need to check if there is enough space for more node locations (written when new edges are read)
    node_loc_t* nodes_loc_buffer_wram = wram_buffer_ptr + max_edges_in_wram_cache * sizeof(edge_t);

    //Indexes to traverse the WRAM buffers
    uint32_t nodes_loc_buffer_index = 0;
    uint32_t sample_buffer_index = max_edges_in_wram_cache;  //At max at first to have a read at first

    uint32_t previous_node_id;

    uint32_t local_read_offset;  //Where the currently considered edges were read from the sample
    uint32_t edges_read = 0;
    bool is_first_read = true;

    //Used only the first node of each edge
    uint32_t current_node_id;
    uint32_t index_in_sample;  //Index of the first occurrence of the unique node inside tha sample

    uint32_t local_unique_nodes = 0;  //Number of unique nodes found

    //The tasklets read the sample in order, after the previous one has read its section
    //The first tasklets starts, then all the other can proceed in order
    if(me() != 0){
        handshake_wait_for(me()-1);
    }

    //Search all the unique nodes considering only the first nodes of the edges
    //Read untill there are edges in the local buffer or there are edges in the sample
    while(sample_buffer_index < edges_read || global_read_offset < edges_in_sample){

        //Transfer some edges of the sample to the WRAM
        if(sample_buffer_index == max_edges_in_wram_cache){

            /*WRITE TO THE MRAM*/
            if(!is_first_read){  //Nothing to write at first
                write_nodes_loc(&nodes_loc_buffer_index, nodes_loc_buffer_wram, AFTER_SAMPLE_HEAP_POINTER, false);
            }

            /*READ FROM THE MRAM*/
            local_read_offset = read_from_sample(sample, edges_in_sample, sample_buffer_wram, &previous_node_id, &edges_read);
            sample_buffer_index = 0;

            //It is necessary to have a second hand notification at first
            //One for the initial wait and one for the read (not for the first tasklet)
            if(is_first_read){
                if(NR_TASKLETS > 1 && me() != NR_TASKLETS-1){
                    handshake_notify();
                }
                is_first_read = false;
            }
        }

        if(edges_read == 0){  //No more edges left
            break;
        }

        //Skip all edges already considered in a previous node location
        //Skip until there are edges to consider and, if there is a previous node id, the current node id is the same
        do{
            current_node_id = sample_buffer_wram[sample_buffer_index].u;
            sample_buffer_index++;
        }while(sample_buffer_index < edges_read && local_read_offset != 0 && current_node_id == previous_node_id);

        //If the last node in the buffer is reached, see if it should be considered or not according to the same conditions as above
        if(sample_buffer_index == edges_read && local_read_offset != 0 && current_node_id == previous_node_id){
                continue;
        }

        //Position inside the MRAM sample
        index_in_sample = local_read_offset + sample_buffer_index - 1;
        local_unique_nodes++;

        //Count the number of nodes that have the current_node as value u in the edge
        //The result is the number of neighbors of that node
        //This is possible because the sample is ordered, there are no duplicates and u<v in each edge
        while(sample_buffer_index < edges_read && sample_buffer_wram[sample_buffer_index].u == current_node_id){
            sample_buffer_index++;
        }

        //Transfer the data to the MRAM
        node_loc_t current_node_info = (node_loc_t) {current_node_id, index_in_sample};
        nodes_loc_buffer_wram[nodes_loc_buffer_index] = current_node_info;
        nodes_loc_buffer_index++;

        //There will be no need to transfer node locations while considering the current edge buffer
    }

    //If the tasklet had nothing to read, it would arrive before the other, changing the order
    //For this reason, it is nececssary to handle this change
    if(is_first_read){

        //Wait for the second norification that is normally used to perform the first read
        if(NR_TASKLETS > 1){
            if(me() == 0){
                handshake_wait_for(NR_TASKLETS-1);
            }else{
                handshake_wait_for(me()-1);
            }
        }

        //Notify next tasklet in line
        //One notification to unlock the first wait
        //A second notification (not for the first tasklet) with the same behaviour as above
        if(NR_TASKLETS > 1){
            handshake_notify();
            if(me() != NR_TASKLETS-1){
                handshake_notify();
            }
        }
    }

    //Write the last node locations
    write_nodes_loc(&nodes_loc_buffer_index, nodes_loc_buffer_wram, AFTER_SAMPLE_HEAP_POINTER, true);

    return local_unique_nodes;
}

void write_nodes_loc(uint32_t* nodes_loc_buffer_index, node_loc_t* nodes_loc_buffer_wram, __mram_ptr node_loc_t* AFTER_SAMPLE_HEAP_POINTER, bool last_write){

    //Tasklets wait in line to write in order to the sample
    if(NR_TASKLETS > 1){
        if(me() == 0){
            handshake_wait_for(NR_TASKLETS-1);
        }else{
            handshake_wait_for(me()-1);
        }
    }

    //Copy back the node locations (if any). The handshake grants the correct order
    if(*nodes_loc_buffer_index > 0){
        uint32_t local_write_offset = global_write_offset;
        global_write_offset += *nodes_loc_buffer_index;

        //Notify next tasklet in line
        if(NR_TASKLETS > 1){
            //If this is the last write, the last tasklet does not notify (infinitely suspended otherwise)
            if(me() != NR_TASKLETS-1 || !last_write){
                handshake_notify();
            }
        }

        write_to_mram(nodes_loc_buffer_wram, AFTER_SAMPLE_HEAP_POINTER + local_write_offset, (*nodes_loc_buffer_index) * sizeof(node_loc_t));
        *nodes_loc_buffer_index = 0;
    }else{
        //Notify next tasklet in line
        if(NR_TASKLETS > 1){
            //If this is the last write, the last tasklet does not notify (infinitely suspended otherwise)
            if(me() != NR_TASKLETS-1 || !last_write){
                handshake_notify();
            }
        }
    }
}

uint32_t read_from_sample(__mram_ptr edge_t* sample, uint32_t edges_in_sample, edge_t* sample_buffer_wram, uint32_t* previous_node_id, uint32_t* edges_read){
    //The first tasklet does not need to wait the first time
    if(global_read_offset != 0 && NR_TASKLETS > 1){
        if(me() == 0){
            handshake_wait_for(NR_TASKLETS-1);
        }else{
            handshake_wait_for(me()-1);
        }
    }

    //Read more edges
    uint32_t local_read_offset = global_read_offset;

    //Read, if present, the node id of the previous edge (if present).
    //Do not create a new node location for a node id already considered

    if(local_read_offset != 0){
        edge_t previous_edge;
        mram_read(&sample[local_read_offset-1], &previous_edge, sizeof(edge_t));
        *previous_node_id = previous_edge.u;
    } //If local_read_offset == 0, the previous_node_id will not be considered

    uint32_t edges_to_read;
    uint32_t max_edges_in_wram_cache = (WRAM_BUFFER_SIZE / sizeof(edge_t)) >> 1;

    if(edges_in_sample - global_read_offset >= max_edges_in_wram_cache){
        edges_to_read = max_edges_in_wram_cache;
    }else{
        edges_to_read = edges_in_sample - global_read_offset;
    }

    *edges_read = edges_to_read;
    global_read_offset += edges_to_read;

    //Notify next tasklet in line
    if(NR_TASKLETS > 1){
        handshake_notify();
    }

    //If edges_in_sample is not a multiple of max_edges_in_wram_cache edges, some bytes will be read from MRAM
    //Containing useless informations. This is not a problem because the loop will stop with certain indexes
    read_from_mram(&sample[local_read_offset], sample_buffer_wram, edges_to_read * sizeof(edge_t));

    return local_read_offset;
}

//No need to use WRAM buffer because it is only for debugging
void print_node_locations(uint32_t number_of_nodes, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER){
    printf("Printing the node location informations. There are %d unique nodes:\n", number_of_nodes);

    for(uint32_t i = 0; i < number_of_nodes; i++){
        node_loc_t current_node;
        mram_read(AFTER_SAMPLE_HEAP_POINTER + i * sizeof(node_loc_t), &current_node, sizeof(node_loc_t));  //Read the informations of one node from MRAM
        printf("Id: %d Index in sample: %d\n", current_node.id, current_node.index_in_sample);
    }
}
