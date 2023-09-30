#include <assert.h> //Assert
#include <stdint.h> //Fixed size integers
#include <stdlib.h> //NULL

#include "common.h"

uint32_t get_node_color(uint32_t node_id, dpu_arguments_t* dpu_input_arguments_ptr){
    assert(dpu_input_arguments_ptr != NULL);

    uint32_t p = dpu_input_arguments_ptr -> hash_parameter_p;
    uint32_t a = dpu_input_arguments_ptr -> hash_parameter_a;
    uint32_t b = dpu_input_arguments_ptr -> hash_parameter_b;

    return ((a * node_id + b) % p) % (dpu_input_arguments_ptr -> n_colors);
}
