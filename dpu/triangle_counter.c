#include <stdlib.h>  //Various things
#include <stdio.h>  //Mainly debug messages
#include <mram.h>  //Transfer data between WRAM and MRAM. Access MRAM
#include <mutex.h>  //Mutex for tasklets

#include "triangle_counter.h"
#include "dpu_util.h"
#include "locate_nodes.h"

bool is_triplet_handled(uint32_t n1, uint32_t n2, uint32_t n3, triplet_t handled_triplet, dpu_arguments_t* DPU_INPUT_ARGUMENTS_PTR){
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

uint32_t global_sample_read_offset = 0;
MUTEX_INIT(offset_sample);

uint32_t count_triangles(__mram_ptr edge_t* sample, uint32_t edges_in_sample, triplet_t handled_triplet, uint32_t num_locations, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER, void* wram_buffer_ptr, dpu_arguments_t* DPU_INPUT_ARGUMENTS_PTR){
    uint32_t count = 0;

    //Create a buffer in the WRAM to read more than one edge from the sample (1/8 of the total space)
    uint32_t max_nr_edges_read = (WRAM_BUFFER_SIZE >> 3) / sizeof(edge_t) ;
    edge_t* edges_read_buffer = (edge_t*) wram_buffer_ptr;
    uint32_t edges_to_read = 0;

    //Create a buffer in the WRAM of the sample with edges starting with u (7/16) and v (7/16) to speed up research
    uint32_t max_edges_in_wram_cache = ((WRAM_BUFFER_SIZE - (WRAM_BUFFER_SIZE >> 3)) >> 1) / sizeof(edge_t);  //Divide by 2 with right shift
    edge_t* u_sample_buffer_wram = (edge_t*) wram_buffer_ptr + max_nr_edges_read;
    edge_t* v_sample_buffer_wram = (edge_t*) wram_buffer_ptr + max_nr_edges_read + max_edges_in_wram_cache;

    uint32_t local_sample_read_offset;
    uint32_t edges_read_buffer_offset = max_nr_edges_read;

    while(edges_read_buffer_offset < edges_to_read || global_sample_read_offset < edges_in_sample){

        if(edges_read_buffer_offset == max_nr_edges_read){

            mutex_lock(offset_sample);

            //The tasklets consider a few edges at a instant
            if(edges_in_sample - global_sample_read_offset >= max_nr_edges_read){
                edges_to_read = max_nr_edges_read;
            }else{
                edges_to_read = edges_in_sample - global_sample_read_offset;
            }

            //It may be possible that all the edges become read while waiting for the mutex
            if(edges_to_read == 0){
                mutex_unlock(offset_sample);
                break;
            }

            local_sample_read_offset = global_sample_read_offset;
            global_sample_read_offset += edges_to_read;
            mutex_unlock(offset_sample);

            read_from_mram(&sample[local_sample_read_offset], edges_read_buffer, edges_to_read * sizeof(edge_t));
            edges_read_buffer_offset = 0;
        }

        edge_t current_edge = edges_read_buffer[edges_read_buffer_offset];
        edges_read_buffer_offset++;

        uint32_t u = current_edge.u;
        uint32_t v = current_edge.v;

        node_loc_t u_info = get_location_info(num_locations, u, AFTER_SAMPLE_HEAP_POINTER);
        node_loc_t v_info = get_location_info(num_locations, v, AFTER_SAMPLE_HEAP_POINTER);

        if(v_info.index_in_sample == -1){  //There is no other edge with v as first node
            continue;
        }

        uint32_t u_index = u_info.index_in_sample;  //Location in sample of the first occurrences of u and v as first nodes of an edge
        uint32_t v_index = v_info.index_in_sample;

        uint32_t u_offset = 0;  //Offsets in the from the indexes in the sample
        uint32_t v_offset = 0;

        uint32_t u_neighbor_id;
        uint32_t v_neighbor_id;

        uint32_t u_sample_buffer_index = 0;
        uint32_t v_sample_buffer_index = 0;

        //It is not a problem if more edges than needed are read (although a bit wasteful)
        read_from_mram(&sample[u_index], u_sample_buffer_wram, max_edges_in_wram_cache * sizeof(edge_t));
        read_from_mram(&sample[v_index], v_sample_buffer_wram, max_edges_in_wram_cache * sizeof(edge_t));

        while(u_sample_buffer_wram[u_sample_buffer_index].u == u && v_sample_buffer_wram[v_sample_buffer_index].u == v){

            //Do not start reading outside the sample region
            //v_offset is continuously updated
            if(v_index + v_offset > edges_in_sample){
                break;
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

            //Retrieve new edges starting with u
            if(u_sample_buffer_index == max_edges_in_wram_cache){
                read_from_mram(&sample[u_index + u_offset], u_sample_buffer_wram, max_edges_in_wram_cache * sizeof(edge_t));
                u_sample_buffer_index = 0;
            }

            //Retrieve new edges starting with v
            if(v_sample_buffer_index == max_edges_in_wram_cache){
                read_from_mram(&sample[v_index + v_offset], v_sample_buffer_wram, max_edges_in_wram_cache * sizeof(edge_t));
                v_sample_buffer_index = 0;
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
        mram_read(AFTER_SAMPLE_HEAP_POINTER + mid * sizeof(node_loc_t), &current_node, sizeof(node_loc_t));  //Read the current node data from the MRAM

        if (current_node.id == node_id) {
            return current_node;

        } else if (current_node.id > node_id) {
            high = mid - 1;

        } else {
            low = mid + 1;
        }
    }
    //Dummy node informations when the node is not present. The only thing that makes this not valid is the number of neighbors at 0
    return (node_loc_t){0,-1};
}
