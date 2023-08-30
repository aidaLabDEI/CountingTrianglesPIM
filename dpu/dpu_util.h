#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>  //Fixed size integers
#include <stdbool.h>  //Booleans
#include <mram.h> //Transfer data between WRAM and MRAM

#include "../common/common.h"

//Size of the heap in the WRAM used as buffer for the MRAM
#define WRAM_BUFFER_SIZE 2048

//Contains a pair of colors, representing the colors of an edge
typedef struct {
    int32_t color_u;
    int32_t color_v;
} edge_colors_t;

//Contains the colors defining a triplet. The colors are ordered
//Signed values to make invalid triplet
typedef struct {
    int32_t color1;
    int32_t color2;
    int32_t color3;
} triplet_t;

//dpu cannot use the standard library random
void srand(uint32_t seed);
uint32_t rand();
uint32_t rand_range(uint32_t from, uint32_t to);  //from and to are included

//Determine what triplet will be used by this DPU and set random number seed
triplet_t initial_setup(uint64_t id, dpu_arguments_t* DPU_INPUT_ARGUMENTS_PTR);

//Hash function to get the color of a node
int32_t get_node_color(uint32_t node_id, dpu_arguments_t* DPU_INPUT_ARGUMENTS_PTR);

//Get ordered colors of the edge
edge_colors_t get_edge_colors(edge_t edge, dpu_arguments_t* DPU_INPUT_ARGUMENTS_PTR);

//The DPU considers if the edge colors are inside a triplet assigned to the DPU
bool is_edge_handled(edge_colors_t edge_colors, triplet_t handled_triplet);

//Wrapper to read from mram to wram. Used to read more than 2048bytes if necessary
void read_from_mram(__mram_ptr void* from_mram, void* to_wram, uint32_t num_bytes);

//Wrapper to write to mram from wram. Used to read more than 2048bytes if necessary
void write_to_mram(void* from_wram, __mram_ptr void* to_mram, uint32_t num_bytes);

//Debug function for printing the content of the sample
void print_sample(__mram_ptr edge_t* sample, uint32_t edges_in_sample);

#endif /* __UTIL_H__ */
