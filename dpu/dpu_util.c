#include <stdlib.h> //Various things
#include <assert.h> //Assert
#include <stdint.h> //Fixed size integers
#include <stdio.h>  //Standard output for debug functions
#include <alloc.h>  //Alloc heap in WRAM
#include <mram.h>  //Transfer data between WRAM and MRAM

#include "dpu_util.h"

//Pseudo-random number generator
uint32_t random_previous = 0;
void srand(uint32_t seed){  //Set seed
    random_previous = seed;
}
uint64_t rand(){
    uint64_t r = RANDOM_A * random_previous + RANDOM_C;
    random_previous = r;
    return r;
}
//From and to are included
uint64_t rand_range(uint32_t from, uint32_t to){
    assert(from <= to);
    return (rand() % (to - from + 1) + from);
}

uint32_t get_node_color(uint32_t node_id, dpu_arguments_t* DPU_INPUT_ARGUMENTS_PTR){
    assert(DPU_INPUT_ARGUMENTS_PTR != NULL);

    uint32_t p = DPU_INPUT_ARGUMENTS_PTR -> HASH_PARAMETER_P;
    uint32_t a = DPU_INPUT_ARGUMENTS_PTR -> HASH_PARAMETER_A;
    uint32_t b = DPU_INPUT_ARGUMENTS_PTR -> HASH_PARAMETER_B;
    return ((a * node_id + b) % p) % (DPU_INPUT_ARGUMENTS_PTR -> N_COLORS);
}

//Need to pass pointer because the handled triplets struct needs to be modified
void initial_setup(uint64_t id, triplets_array_t* handled_triplets_ptr, dpu_arguments_t* DPU_INPUT_ARGUMENTS_PTR){
    assert(id < NR_DPUS);
    assert(handled_triplets_ptr != NULL);
    assert(DPU_INPUT_ARGUMENTS_PTR != NULL);

    mem_reset();  //Reset WRAM heap before starting

    srand(DPU_INPUT_ARGUMENTS_PTR -> random_seed);  //Effect is global

    uint32_t c = DPU_INPUT_ARGUMENTS_PTR -> N_COLORS;

    //Total number of triplets given the number of colors is calculated. t = binom(c+2, 3)
    uint32_t total_triplets = (c * (c+1) * (c+2))/6;

    //Maximum amount of triplets that any dpu will have to handle.
    //Divide the triplets uniformly between the DPUs
    //Cannot use math.h for ceiling, so a formula is used
    uint32_t triplets_per_dpu = (uint32_t)(total_triplets+NR_DPUS-1)/NR_DPUS;

    //The triplets will reside in the WRAM. Better to not use to much space
    //Also having too many triplets in a DPU will lead to poor results (too many comparisons)
    assert(triplets_per_dpu <= 32);

    //Create an array that will store the triplets.
    //Created in the heap of the WRAM to be persistent and because frequently accessed
    //If there are more triplets per dpu, this solution will result in, at maximum, sizeof(triplet_t) space wasted
    triplet_t* temp_triplets_array = (triplet_t*) mem_alloc(triplets_per_dpu * sizeof(triplet_t));

    uint32_t count = 0;  //Counting the number of triplets generated
    uint32_t array_index = 0;
    //Create every single possible triplet given the amount of colors
    for(uint32_t c1 = 0; c1 < c; c1++){
		for(uint32_t c2 = c1; c2 < c; c2++){
			for(uint32_t c3 = c2; c3 < c; c3++){

                //If the triplet is assigned to this DPU
                if(count%NR_DPUS == id){
                    triplet_t current_triplet = {c1, c2, c3};
                    temp_triplets_array[array_index] = current_triplet;
                    array_index++;
                }
                count++;
			}
		}
	}

    //Assign values to the global variable
    handled_triplets_ptr->is_set = true;
    handled_triplets_ptr->size = array_index;
    handled_triplets_ptr->array = temp_triplets_array;
}

//The returned colors are ordered
edge_colors_t get_edge_colors(edge_t edge, dpu_arguments_t* DPU_INPUT_ARGUMENTS_PTR){
    assert(edge.u != edge.v);
    assert(DPU_INPUT_ARGUMENTS_PTR != NULL);

    uint32_t color_u = get_node_color(edge.u, DPU_INPUT_ARGUMENTS_PTR);
    uint32_t color_v = get_node_color(edge.v, DPU_INPUT_ARGUMENTS_PTR);

    //The colors must be ordered
    if(color_u < color_v){
        return (edge_colors_t){color_u, color_v};
    }
    return (edge_colors_t){color_v, color_u};
}

bool is_edge_handled(edge_colors_t edge_colors, triplets_array_t* handled_triplets_ptr){
    assert(edge_colors.color_u <= edge_colors.color_v); //Colors need to be ordered
    assert(handled_triplets_ptr != NULL);

    for(uint32_t i = 0; i < handled_triplets_ptr->size; i++){ //For every triplet
        triplet_t current_triplet = handled_triplets_ptr->array[i];

        //Check every possible combination (possible because colors are ordered)
        if((edge_colors.color_u == current_triplet.color1 && edge_colors.color_v == current_triplet.color2) ||
            (edge_colors.color_u == current_triplet.color1 && edge_colors.color_v == current_triplet.color3) ||
            (edge_colors.color_u == current_triplet.color2 && edge_colors.color_v == current_triplet.color3)){
                return true;
        }
    }
    return false;
}

void read_from_mram(__mram_ptr void* from_mram, void* to_wram, uint32_t num_bytes){
    assert(from_mram != NULL);
    assert(to_wram != NULL);
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
    assert(from_wram != NULL);
    assert(to_mram != NULL);
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

//Debug function for printing the sample
void print_sample(__mram_ptr edge_t* sample, uint32_t edges_in_sample){
    assert(sample != NULL);

    printf("Printing the sample with %d edges:\n", edges_in_sample);

    edge_t current_edge;
    for(uint32_t i = 0; i < edges_in_sample; i++){
        current_edge = sample[i];
        printf("%d %d\n", current_edge.u, current_edge.v);
    }
}

//Debug function for printing the triplets handled by the DPU
void print_handled_triplets(triplets_array_t* handled_triplets_ptr){
    assert(handled_triplets_ptr != NULL);

    printf("Printing the handled triplets that are %d:\n", handled_triplets_ptr -> size);

    for(uint32_t i = 0; i < handled_triplets_ptr -> size; i++){

        triplet_t current_triplet = handled_triplets_ptr->array[i];
        printf("%d %d %d\n", current_triplet.color1, current_triplet.color2, current_triplet.color3);
    }
}
