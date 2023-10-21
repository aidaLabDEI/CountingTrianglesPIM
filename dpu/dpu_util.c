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
            mutex_unlock(offset_sample_max_id);
            break;
        }

        local_sample_offset = global_sample_offset;
        global_sample_offset += edges_to_read;
        mutex_unlock(offset_sample_max_id);

        mram_read(&sample[local_sample_offset], wram_edges_buffer, edges_to_read * sizeof(edge_t));

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
