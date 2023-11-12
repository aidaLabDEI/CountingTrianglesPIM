#include <stdlib.h>  //Various things
#include <stdio.h>  //Mainly debug messages
#include <mram.h>  //Transfer data between WRAM and MRAM. Access MRAM
#include <mutex.h>  //Mutex for tasklets
#include <defs.h>

#include "triangle_counter.h"
#include "dpu_util.h"
#include "locate_nodes.h"
#include "../common/common.h"

uint32_t global_sample_read_offset = 0;
MUTEX_INIT(offset_sample);

uint32_t count_triangles(__mram_ptr edge_t* sample, uint32_t edges_in_sample, uint32_t num_locations, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER, void* wram_buffer_ptr){
    uint32_t count = 0;

    //Create a buffer in the WRAM to read more than one edge from the sample (7/8 of the total space)
    //Bigger buffer for the edge to consider than the buffers to store traversed edges because traversed edges
    //may be just a few and it would be a waste to load a lot of data that will not be used.
    //Better to read more single edges to consider to enter less frequently in the mutex
    uint32_t max_nr_edges_read = (WRAM_BUFFER_SIZE - (WRAM_BUFFER_SIZE >> 3)) / sizeof(edge_t) ;
    edge_t* edges_read_buffer = (edge_t*) wram_buffer_ptr;
    uint32_t edges_to_read = 0;

    //Create a buffer in the WRAM of the sample with edges starting with u (1/16) and v (1/16) to speed up research
    uint32_t max_edges_in_wram_cache = (WRAM_BUFFER_SIZE >> 4) / sizeof(edge_t);  //Find 1/8 and divide by 2
    edge_t* u_sample_buffer_wram = (edge_t*) wram_buffer_ptr + max_nr_edges_read;
    edge_t* v_sample_buffer_wram = (edge_t*) wram_buffer_ptr + max_nr_edges_read + max_edges_in_wram_cache;

    //If not needed as a the sample buffer, it can be used for other purposes
    //Given a buffer of size N bytes, the cycles needed without a buffer are log2(N/8) * (77 + 0.5 * 8), with a buffer (77 + 0.5 * N)
    //A buffer gives better results with N between 24 and 960
    uint32_t max_node_loc_in_wram_cache = (WRAM_BUFFER_SIZE >> 3) / sizeof(node_loc_t);
    node_loc_t* bin_search_buffer = (node_loc_t*)wram_buffer_ptr + max_nr_edges_read;

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

            mram_read(&sample[local_sample_read_offset], edges_read_buffer, edges_to_read * sizeof(edge_t));
            edges_read_buffer_offset = 0;
        }

        edge_t current_edge = edges_read_buffer[edges_read_buffer_offset];
        edges_read_buffer_offset++;

        uint32_t u = current_edge.u;
        uint32_t v = current_edge.v;

        //No need to find the u_info because the starting location is given by the address of the current edge
        node_loc_t v_info = get_location_info(num_locations, v, AFTER_SAMPLE_HEAP_POINTER, bin_search_buffer, max_node_loc_in_wram_cache);

        if(v_info.index_in_sample == -1){  //There is no other edge with v as first node
            continue;
        }

        uint32_t u_index = local_sample_read_offset + edges_read_buffer_offset - 1;
        uint32_t v_index = v_info.index_in_sample;  //Location in sample of the first occurrences of v as first nodes of an edge

        uint32_t u_offset = 0;  //Offsets in the from the indexes in the sample
        uint32_t v_offset = 0;

        uint32_t u_neighbor_id;
        uint32_t v_neighbor_id;

        uint32_t v_sample_buffer_index = 0;
        mram_read(&sample[v_index], v_sample_buffer_wram, max_edges_in_wram_cache * sizeof(edge_t));

        //Use the edges that are already present in the WRAM at first
        uint32_t u_edges_to_copy;
        uint32_t u_sample_buffer_index;

        //If in the edges buffer there are more edges than there can be in the u_buffer, copy everything that fits
        if(max_nr_edges_read - (edges_read_buffer_offset-1) > max_edges_in_wram_cache){
            u_edges_to_copy = max_edges_in_wram_cache;
            u_sample_buffer_index = 0;
        }else{
            //Copy only the last edges in the buffer
            u_edges_to_copy = max_nr_edges_read - (edges_read_buffer_offset-1);
            u_sample_buffer_index = max_edges_in_wram_cache - u_edges_to_copy;
        }

        for(uint32_t i = 0; i < u_edges_to_copy; i++){
            u_sample_buffer_wram[u_sample_buffer_index + i] = edges_read_buffer[edges_read_buffer_offset - 1 + i];
        }

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
                //It does not matter if a triangle is counted in multiple DPUs. The results is adjusted considering this
                count++;

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
                mram_read(&sample[u_index + u_offset], u_sample_buffer_wram, max_edges_in_wram_cache * sizeof(edge_t));
                u_sample_buffer_index = 0;
            }

            //Retrieve new edges starting with v
            if(v_sample_buffer_index == max_edges_in_wram_cache){
                mram_read(&sample[v_index + v_offset], v_sample_buffer_wram, max_edges_in_wram_cache * sizeof(edge_t));
                v_sample_buffer_index = 0;
            }
        }
    }

    return count;
}

//Not much benefit transferring much more data to the WRAM than the one that is necessary
node_loc_t get_location_info(uint32_t unique_nodes, uint32_t node_id, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER, node_loc_t* node_loc_buffer_ptr, uint32_t max_node_loc_in_buffer){

    int low = 0, high = unique_nodes - 1;

    while (low <= high) {

        //If there are more elements than the maximum that can fit in the WRAM buffer, do normal binary search
        if((uint32_t)(high - low + 1) > max_node_loc_in_buffer){
            node_loc_t current_node;

            int mid = (low + high) >> 1;  //Divide by 2 with right shift
            mram_read((__mram_ptr void*) (AFTER_SAMPLE_HEAP_POINTER + mid * sizeof(node_loc_t)), &current_node, sizeof(node_loc_t));  //Read the current node data from the MRAM

            if (current_node.id == node_id) {
                return current_node;

            } else if (current_node.id > node_id) {
                high = mid - 1;

            } else {
                low = mid + 1;
            }
        }else{
            //Do not load more elements than necessary
            uint32_t node_loc_to_load = (high - low + 1);

            //Search in the remaining elements
            mram_read((__mram_ptr void*) (AFTER_SAMPLE_HEAP_POINTER + low * sizeof(node_loc_t)), node_loc_buffer_ptr, node_loc_to_load * sizeof(node_loc_t));  //Read the current node data from the MRAM

            return get_location_info_WRAM(node_id, node_loc_buffer_ptr, node_loc_to_load);
        }
    }
    //Dummy node informations when the node is not present. The only thing that makes this not valid is the number of neighbors at 0
    return (node_loc_t){0,-1};
}

node_loc_t get_location_info_WRAM(uint32_t node_id, node_loc_t* node_loc_buffer_ptr, uint32_t num_elements){
    int low = 0, high = num_elements - 1;

    while (low <= high) {

        int mid = (low + high) >> 1;  //Divide by 2 with right shift
        node_loc_t current_node = node_loc_buffer_ptr[mid];

        if(current_node.id == node_id){
            return current_node;

        }else if (current_node.id > node_id){
            high = mid - 1;

        }else{
            low = mid + 1;
        }
    }
    //Dummy node informations when the node is not present. The only thing that makes this not valid is the number of neighbors at 0
    return (node_loc_t){0,-1};
}
