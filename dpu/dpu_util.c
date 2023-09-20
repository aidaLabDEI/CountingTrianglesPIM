#include <stdlib.h> //Various things
#include <assert.h> //Assert
#include <stdint.h> //Fixed size integers
#include <stdio.h>  //Standard output for debug functions
#include <alloc.h>  //Alloc heap in WRAM
#include <mram.h>  //Transfer data between WRAM and MRAM
#include <mutex.h>  //Mutex for tasklets

#include "dpu_util.h"

//Pseudo-random number generator
uint32_t random_previous = 0;
void srand(uint32_t seed){  //Set seed
    random_previous = seed;
}
uint32_t rand(){
    uint32_t r = 214013 * random_previous + 2531011;  //MS rand
    random_previous = r;
    return r;
}
//From and to are included
uint32_t rand_range(uint32_t from, uint32_t to){
    assert(from <= to);
    return (rand() % (to - from + 1) + from);
}

int32_t get_node_color(uint32_t node_id, dpu_arguments_t* DPU_INPUT_ARGUMENTS_PTR){
    assert(DPU_INPUT_ARGUMENTS_PTR != NULL);

    uint32_t p = DPU_INPUT_ARGUMENTS_PTR -> hash_parameter_p;
    uint32_t a = DPU_INPUT_ARGUMENTS_PTR -> hash_parameter_a;
    uint32_t b = DPU_INPUT_ARGUMENTS_PTR -> hash_parameter_b;
    return ((a * node_id + b) % p) % (DPU_INPUT_ARGUMENTS_PTR -> n_colors);
}

triplet_t initial_setup(uint64_t id, dpu_arguments_t* DPU_INPUT_ARGUMENTS_PTR){
    assert(id < NR_DPUS);
    assert(DPU_INPUT_ARGUMENTS_PTR != NULL);

    mem_reset();  //Reset WRAM heap before starting

    srand(DPU_INPUT_ARGUMENTS_PTR -> random_seed);  //Effect is global

    uint32_t count = 0;  //Counting the number of triplets generated

    uint32_t c = DPU_INPUT_ARGUMENTS_PTR -> n_colors;

    //Create every single possible triplet given the amount of colors
    for(uint32_t c1 = 0; c1 < c; c1++){
		for(uint32_t c2 = c1; c2 < c; c2++){
			for(uint32_t c3 = c2; c3 < c; c3++){

                //If the triplet is assigned to this DPU
                if(count == id){
                    return (triplet_t){c1, c2, c3};
                }
                count++;
			}
		}
	}

    return (triplet_t){-1, -1, -1};
}

//The returned colors are ordered
edge_colors_t get_edge_colors(edge_t edge, dpu_arguments_t* DPU_INPUT_ARGUMENTS_PTR){
    assert(edge.u != edge.v);
    assert(DPU_INPUT_ARGUMENTS_PTR != NULL);

    int32_t color_u = get_node_color(edge.u, DPU_INPUT_ARGUMENTS_PTR);
    int32_t color_v = get_node_color(edge.v, DPU_INPUT_ARGUMENTS_PTR);

    //The colors must be ordered
    if(color_u < color_v){
        return (edge_colors_t){color_u, color_v};
    }
    return (edge_colors_t){color_v, color_u};
}

bool is_edge_handled(edge_colors_t edge_colors, triplet_t handled_triplet){
    assert(edge_colors.color_u <= edge_colors.color_v); //Colors need to be ordered

    //Check every possible combination (possible because colors are ordered)
    if((edge_colors.color_u == handled_triplet.color1 && edge_colors.color_v == handled_triplet.color2) ||
        (edge_colors.color_u == handled_triplet.color1 && edge_colors.color_v == handled_triplet.color3) ||
        (edge_colors.color_u == handled_triplet.color2 && edge_colors.color_v == handled_triplet.color3)){
            return true;
    }

    return false;
}

void read_from_mram(__mram_ptr void* from_mram, void* to_wram, uint32_t num_bytes){
    assert(num_bytes <= WRAM_BUFFER_SIZE);

    uint32_t max_transfer_size = 2048;  //Set by the library
    for(uint32_t t = 0; t < num_bytes; t += max_transfer_size){  //t indicate the number of transferred bytes

        uint32_t still_to_transfer = num_bytes - t;

        if(still_to_transfer >= max_transfer_size){
            mram_read(from_mram + t, to_wram + t, max_transfer_size);  //Offset in bytes because pointers are void*
        }else{
            mram_read(from_mram + t, to_wram + t, still_to_transfer);
        }
    }
}

void write_to_mram(void* from_wram, __mram_ptr void* to_mram, uint32_t num_bytes){
    assert(num_bytes <= WRAM_BUFFER_SIZE);

    uint32_t max_transfer_size = 2048; //Set by the library
    for(uint32_t t = 0; t < num_bytes; t += max_transfer_size){  //t indicate the number of transferred bytes

        uint32_t still_to_transfer = num_bytes - t;

        if(still_to_transfer >= max_transfer_size){
            mram_write(from_wram + t, to_mram + t, max_transfer_size);   //Offset in bytes because pointers are void*
        }else{
            mram_write(from_wram + t, to_mram + t, still_to_transfer);
        }
    }
}

uint32_t global_sample_offset = 0;
MUTEX_INIT(offset_sample_max_id);

uint32_t determine_max_node_id(__mram_ptr edge_t* sample, uint32_t edges_in_sample, edge_t* wram_edges_buffer){

    uint32_t max_nr_edges_read = WRAM_BUFFER_SIZE/sizeof(edge_t);
    uint32_t local_sample_offset = 0;

    uint32_t edges_to_read = 0;

    uint32_t max_node_id = 0;

    while(global_sample_offset < edges_in_sample){

        mutex_lock(offset_sample_max_id);

        if(edges_in_sample - global_sample_offset >= max_nr_edges_read){
            edges_to_read = max_nr_edges_read;
        }else{
            edges_to_read = edges_in_sample - global_sample_offset;
        }

        //It may be possible that all the edges become read while waiting for the mutex
        if(edges_to_read == 0){
            break;
        }

        local_sample_offset = global_sample_offset;
        global_sample_offset += edges_to_read;
        mutex_unlock(offset_sample_max_id);

        read_from_mram(&sample[local_sample_offset], wram_edges_buffer, edges_to_read * sizeof(edge_t));

        for(uint32_t i = 0; i < edges_to_read; i++){
            if(wram_edges_buffer[i].v > max_node_id){  //Checking only the second node is enough because the nodes in an edge are ordered
                max_node_id = wram_edges_buffer[i].v;
            }
        }
    }

    return max_node_id;
}

//Debug function for printing the sample
void print_sample(__mram_ptr edge_t* sample, uint32_t edges_in_sample){
    printf("Printing the sample with %d edges:\n", edges_in_sample);

    edge_t current_edge;
    for(uint32_t i = 0; i < edges_in_sample; i++){
        current_edge = sample[i];
        printf("%d %d\n", current_edge.u, current_edge.v);
    }
}
