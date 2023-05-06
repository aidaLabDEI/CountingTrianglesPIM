#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>  //Fixed size integers
#include <stdbool.h>  //Booleans
#include <mram.h> //Transfer data between WRAM and MRAM

#include "../common/common.h"

//Size of the heap in the WRAM used as buffer for the MRAM
#define WRAM_BUFFER_SIZE 2048

//Values used to calculate pseudo-random numbers
#define RANDOM_A 25214903917
#define RANDOM_C 11

//Contains a pair of colors, representing the colors of an edge
typedef struct {
    uint32_t color_u;
    uint32_t color_v;
} edge_colors_t;

//Contains the colors defining a triplet. The colors are ordered
typedef struct {
    uint32_t color1;
    uint32_t color2;
    uint32_t color3;
} triplet_t;

//Store both the triplets handled by a dpu and their number.
//is_set is used to determine if it is the first execution of the kernel or not,
//Using size != 0 would not work when no triplets are assigned to this DPU
typedef struct {
    bool is_set;
    uint32_t size;
    triplet_t* array;
} triplets_array_t;

//dpu cannot use the standard library random
void srand(uint32_t seed);
uint64_t rand();
uint64_t rand_range(uint32_t from, uint32_t to);  //from and to are included

//Determine what triplets will be used by this DPU and set random number seed
void initial_setup(uint64_t id, triplets_array_t* handled_triplets_ptr, dpu_arguments_t* DPU_INPUT_ARGUMENTS_PTR);

//Hash function to get the color of a node
uint32_t get_node_color(uint32_t node_id, dpu_arguments_t* DPU_INPUT_ARGUMENTS_PTR);

//Get ordered colors of the edge
edge_colors_t get_edge_colors(edge_t edge, dpu_arguments_t* DPU_INPUT_ARGUMENTS_PTR);

//The DPU considers if the edge colors are inside a triplet assigned to the DPU
bool is_edge_handled(edge_colors_t edge_colors, triplets_array_t* handled_triplets_ptr);

//Wrapper to read from mram to wram. Used to read more than 2048bytes if necessary
void read_from_mram(__mram_ptr void* from_mram, void* to_wram, uint32_t num_bytes);

//Wrapper to write to mram from wram. Used to read more than 2048bytes if necessary
void write_to_mram(void* from_wram, __mram_ptr void* to_mram, uint32_t num_bytes);

//Debug function for printing the content of the sample
void print_sample(__mram_ptr edge_t* sample, uint32_t edges_in_sample);

//Debug function for printing the triplets handled by the DPU
void print_handled_triplets(triplets_array_t* handled_triplets_ptr);

#endif /* __UTIL_H__ */
