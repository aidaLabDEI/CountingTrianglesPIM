#include <assert.h>  //Assert
#include <stdlib.h>  //Various things
#include <stdio.h>  //Mainly debug messages
#include <mram.h>  //Transfer data between WRAM and MRAM. Access MRAM

#include "triangle_counter.h"
#include "dpu_util.h"
#include "locate_nodes.h"

bool is_triplet_handled(uint32_t n1, uint32_t n2, uint32_t n3, triplet_t handled_triplet, dpu_arguments_t* DPU_INPUT_ARGUMENTS_PTR){
    assert(DPU_INPUT_ARGUMENTS_PTR != NULL);

    int32_t c1 = get_node_color(n1, DPU_INPUT_ARGUMENTS_PTR);
    int32_t c2 = get_node_color(n2, DPU_INPUT_ARGUMENTS_PTR);
    int32_t c3 = get_node_color(n3, DPU_INPUT_ARGUMENTS_PTR);
    //Ordering the colors c1 <= c2 <= c3
    if (c1 > c2) { int32_t temp = c1; c1 = c2; c2 = temp; }
    if (c1 > c3) { int32_t temp = c1; c1 = c3; c3 = temp; }
    if (c2 > c3) { int32_t temp = c2; c2 = c3; c3 = temp; }

    if(handled_triplet.color1 == c1 && handled_triplet.color2 == c2 && handled_triplet.color3 == c3){
        return true;
    }

    return false;
}

uint32_t count_triangles(__mram_ptr edge_t* sample, uint32_t from, uint32_t to, triplet_t handled_triplet, uint32_t num_locations, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER, void* wram_buffer_ptr, dpu_arguments_t* DPU_INPUT_ARGUMENTS_PTR){
    assert(from <= to);  //Equal if the tasklet does not have edges assigned to it
    assert(DPU_INPUT_ARGUMENTS_PTR != NULL);

    uint32_t count = 0;

    //Create a buffer in the WRAM of the sample with edges starting with u and v to speed up research
    uint32_t edges_in_wram_cache = (WRAM_BUFFER_SIZE / sizeof(edge_t)) >> 1;  //Divide by 2 with right shift
    edge_t* u_sample_buffer_wram = (edge_t*) wram_buffer_ptr;
    edge_t* v_sample_buffer_wram = (edge_t*) wram_buffer_ptr + edges_in_wram_cache;

    for(uint32_t i = from; i < to; i++){
        edge_t current_edge = sample[i];
        uint32_t u = current_edge.u;
        uint32_t v = current_edge.v;

        node_loc_t u_info = get_location_info(num_locations, u, AFTER_SAMPLE_HEAP_POINTER);
        node_loc_t v_info = get_location_info(num_locations, v, AFTER_SAMPLE_HEAP_POINTER);

        if(v_info.number_bigger_neighbors == 0){  //There is no other edge with v as first node
            continue;
        }

        uint32_t u_offset = 0;  //index offset inside the sample starting from the first occurrence of u and v
        uint32_t v_offset = 0;

        uint32_t u_index = u_info.index_in_sample;  //Location in sample of the first occurrences of u and v as first nodes of an edge
        uint32_t v_index = v_info.index_in_sample;

        uint32_t u_neighbor_id;
        uint32_t v_neighbor_id;

        uint32_t u_sample_buffer_index = edges_in_wram_cache;  //Set to max value to force a read in the first iteration
        uint32_t v_sample_buffer_index = edges_in_wram_cache;

        while(u_offset < u_info.number_bigger_neighbors && v_offset < v_info.number_bigger_neighbors){

            //Retrieve new edges starting with u
            if(u_sample_buffer_index == edges_in_wram_cache){
                //Retrieve only the necessary amount of edges
                uint32_t edges_to_retrieve = edges_in_wram_cache < u_info.number_bigger_neighbors ? edges_in_wram_cache : u_info.number_bigger_neighbors;
                read_from_mram(&sample[u_index + u_offset], u_sample_buffer_wram, edges_to_retrieve * sizeof(edge_t));
                u_sample_buffer_index = 0;
            }

            //Retrieve new edges starting with v
            if(v_sample_buffer_index == edges_in_wram_cache){
                //Retrieve only the necessary amount of edges
                uint32_t edges_to_retrieve = edges_in_wram_cache < v_info.number_bigger_neighbors ? edges_in_wram_cache : v_info.number_bigger_neighbors;
                read_from_mram(&sample[v_index + v_offset], v_sample_buffer_wram, edges_to_retrieve * sizeof(edge_t));
                v_sample_buffer_index = 0;
            }

            u_neighbor_id = u_sample_buffer_wram[u_sample_buffer_index].v;
            v_neighbor_id = v_sample_buffer_wram[v_sample_buffer_index].v;

            //Because the edges are ordered, it is possible to efficiently traverse the sample
            if(u_neighbor_id == v_neighbor_id){
                if(is_triplet_handled(u, v, u_neighbor_id, handled_triplet, DPU_INPUT_ARGUMENTS_PTR)){
                    count++;
                }
                u_offset++;
                v_offset++;

                u_sample_buffer_index++;
                v_sample_buffer_index++;
            }

            if(u_neighbor_id < v_neighbor_id){
                u_offset++;
                u_sample_buffer_index++;
            }

            if(u_neighbor_id > v_neighbor_id){
                v_offset++;
                v_sample_buffer_index++;
            }
        }
    }

    return count;
}

//Not much benefit transferring much more data to the WRAM than the one that is necessary
node_loc_t get_location_info(uint32_t unique_nodes, uint32_t node_id, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER){

    int low = 0, high = unique_nodes - 1;

    while (low <= high) {
        node_loc_t current_node;

        int mid = (low + high) >> 1;  //Divide by 2 with right shift
        mram_read( AFTER_SAMPLE_HEAP_POINTER + mid * sizeof(node_loc_t), &current_node, sizeof(node_loc_t));  //Read the current node data from the MRAM

        if (current_node.id == node_id) {
            return current_node;

        } else if (current_node.id > node_id) {
            high = mid - 1;

        } else {
            low = mid + 1;
        }
    }
    //Dummy node informations when the node is not present. The only thing that makes this not valid is the number of neighbors at 0
    return (node_loc_t){0,0,0,0};
}
