#include <stdlib.h> //Various things
#include <assert.h> //Assert
#include <stdint.h> //Fixed size integers
#include <stdio.h>  //Standard output for debug functions
#include <dpu_types.h>  //Access DPU MRAM heap
#include <math.h>  //Round
#include <stdbool.h>  //Booleans
#include <pthread.h>  //Mutexes

#include "host_util.h"
#include "../common/common.h"

//The returned colors are ordered
edge_colors_t get_edge_colors(edge_t edge, dpu_arguments_t* dpu_input_arguments_ptr){
    assert(edge.u != edge.v);
    assert(dpu_input_arguments_ptr != NULL);

    uint32_t color_u = get_node_color(edge.u, dpu_input_arguments_ptr);
    uint32_t color_v = get_node_color(edge.v, dpu_input_arguments_ptr);

    //The colors must be ordered
    if(color_u < color_v){
        return (edge_colors_t){color_u, color_v};
    }
    return (edge_colors_t){color_v, color_u};
}

void* handle_edges_file(void* args_thread){  //Run in multiple threads

    handle_edges_thread_args_t* args = (handle_edges_thread_args_t*)args_thread;

    //Buffer to read each line. Each node can use 10 chars each at max (unsigned integers of 4 bytes)
    char char_buffer[32];
    uint32_t node1, node2;

    edge_t current_edge;

    uint32_t file_size = args -> file_size;
    char* mmaped_file = args -> mmaped_file;

    uint32_t file_char_counter = args -> from_char;

    //If the division makes the thread start in the middle of an edge, skip to the first full edge
    //The edge skipped will be handled by the thread with previous id
    if(file_char_counter != 0 && mmaped_file[file_char_counter-1] != '\n'){
        for(; file_char_counter < (args -> to_char); file_char_counter++){
            if(mmaped_file[file_char_counter] == '\n'){
                file_char_counter++;
                break;
            }
        }
    }

    while (file_char_counter < args -> to_char) {  //Reads until EOF

        //Read the file char by char until EOL
        for(uint32_t c = 0; c < sizeof(char_buffer) && file_char_counter < file_size; c++){
            if(mmaped_file[file_char_counter] == '\n'){
                file_char_counter++;
                break;
            }

            char_buffer[c] = mmaped_file[file_char_counter];
            file_char_counter++;
        }

        //Each edge is formed by two unsigned integers separated by a space
        sscanf(char_buffer, "%d %d", &node1, &node2);

        //Edges are considered valid from the file (no duplicates, node1 != node2)
        if(node1 < node2){  //Nodes in edge need to be ordered
           current_edge = (edge_t){node1, node2};
        }else{
           current_edge = (edge_t){node2, node1};
        }

        insert_edge_into_batches(current_edge, args -> dpu_set, args -> dpu_info_array, args -> dpu_input_arguments_ptr, args -> send_to_dpu_mutex, args -> th_id);
    }

    pthread_exit(NULL);
}

void insert_edge_into_batches(edge_t current_edge, struct dpu_set_t dpu_set, dpu_info_t* dpu_info_array, dpu_arguments_t* dpu_input_arguments_ptr, pthread_mutex_t* send_to_dpu_mutex, uint32_t th_id){

    //Given that the current edge has colors (a,b), with a <= b
    edge_colors_t current_edge_colors = get_edge_colors(current_edge, dpu_input_arguments_ptr);
    uint32_t a = current_edge_colors.color_u;
    uint32_t b = current_edge_colors.color_v;

    uint32_t colors = dpu_input_arguments_ptr -> n_colors;

    //Considering the current way triplets are assigned to a DPU, to find the ids of the DPUs that will
    //handle the current edge, it is necessary to consider the cases (a, b, c3), (a, c2, b) and (c1, a, b) (not allowing for duplicates)

    //For (a, b, c3), the first occurence has id:
    //[sum from x = 0 to (a-1) 0.5*(colors - x) * (colors - x + 1)] + 0.5 * (colors - a) * (colors - a + 1) - 0.5 * (colors - b) * (colors - b + 1)
    //This formula uses different Gauss sums that show up considering the creation of the triplets
    //After the first occurence, the other ids are the following (+1) until c3 reaches the number of total colors
    uint32_t current_dpu_id = round((1.0/6) * (a*a*a  - 3*a*a*colors + a*(3*colors*colors-1) - 3*b*(b-2*colors-1)));

    for(uint32_t c3 = b; c3 < colors; c3++){  //Varying the third color

        dpu_info_t* current_dpu_info = &dpu_info_array[th_id * NR_DPUS + current_dpu_id];

        (current_dpu_info -> batch)[(current_dpu_info -> edge_count_batch)++] = current_edge;

        //If the batch is full, send everything batch to all DPUs.
        //When a batch is full, it's likely that most are. Sending in parallel is more efficient
        if(current_dpu_info -> edge_count_batch == BATCH_SIZE_EDGES){
            send_batches(th_id, th_id+1, dpu_info_array, send_to_dpu_mutex, dpu_set);
        }

        current_dpu_id++;
    }

    //For (a, c2, b), the first occurence has id:
    //[sum from x = 0 to (a-1) 0.5*(colors - x) * (colors - x + 1)] + b - a
    //This formula uses different Gauss sums that show up considering the creation of the triplets
    //After the first occurence, the other ids are determined adding (colors - 1 - c2)

    current_dpu_id = round((1.0/6) * a * (a*a - 3*a*(colors+1) + 3*colors*colors +6*colors + 2) -a +b);

    for(uint32_t c2 = a; c2 <= b; c2++){  //Varying the third color
        if(c2 != a && c2 != b){  //Avoid duplicate insertion in triplets (y, y, y)

            dpu_info_t* current_dpu_info = &dpu_info_array[th_id * NR_DPUS + current_dpu_id];

            (current_dpu_info -> batch)[(current_dpu_info -> edge_count_batch)++] = current_edge;

            //If the batch is full, send everything batch to all DPUs.
            //When a batch is full, it's likely that most are. Sending in parallel is more efficient
            if(current_dpu_info -> edge_count_batch == BATCH_SIZE_EDGES){
                send_batches(th_id, th_id+1, dpu_info_array, send_to_dpu_mutex, dpu_set);
            }
        }

        current_dpu_id += colors - 1 - c2;
    }

    for(uint32_t c1 = 0; c1 <= a; c1++){
        if(c1 != b || c1 != a){  //Avoid duplicate insertions in the cases already considered

            //Similar idea as before. This time, (c1, a, b)
            //[sum from x=0 to c1-1 0.5*(C-x)*(C-x+1)] + 0.5*(C-c1)*(C-c1+1) - 0.5*(C-a)(C-a+1) + b - a
            current_dpu_id = round(-0.5 * a*a + a*colors - 0.5 * a + b +0.5 *colors*colors * c1 - 0.5 * colors* c1* c1 +(1.0/6) * c1*c1*c1 - (1.0/6)*c1);

            dpu_info_t* current_dpu_info = &dpu_info_array[th_id * NR_DPUS + current_dpu_id];

            (current_dpu_info -> batch)[(current_dpu_info -> edge_count_batch)++] = current_edge;

            //If the batch is full, send everything batch to all DPUs.
            //When a batch is full, it's likely that most are. Sending in parallel is more efficient
            if(current_dpu_info -> edge_count_batch == BATCH_SIZE_EDGES){
                send_batches(th_id, th_id+1, dpu_info_array, send_to_dpu_mutex, dpu_set);
            }

            current_dpu_id += (1.0/2) * (colors - c1) * (colors - c1 + 1);
        }
    }
}

void send_batches(uint32_t th_id_from, uint32_t th_id_to, dpu_info_t* dpu_info_array, pthread_mutex_t* mutex, struct dpu_set_t dpu_set){

    for(uint32_t t = th_id_from; t < th_id_to; t++){

        //Do not send too much useless data
        uint32_t max_batch_size = 0;
        for(int dpu_id = 0; dpu_id < NR_DPUS; dpu_id++){
            if(max_batch_size < dpu_info_array[t * NR_DPUS + dpu_id].edge_count_batch){
                max_batch_size = dpu_info_array[t * NR_DPUS + dpu_id].edge_count_batch;
            }
        }

        //Send data to the DPUs
        pthread_mutex_lock(mutex);

        //Wait for all the DPUs to finish their task.
        //There is no problem if the program was never launched in the DPUs
        DPU_ASSERT(dpu_sync(dpu_set));

        uint32_t index;
        struct dpu_set_t dpu;

        DPU_FOREACH(dpu_set, dpu, index) {
            DPU_ASSERT(dpu_prepare_xfer(dpu, dpu_info_array[t * NR_DPUS + index].batch));  //Associate an address to each dpu
        }

        //When a batch of a DPU in a given thread is full, is likely most of the other will be too. At this point, use parallel transfers
        DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, DPU_MRAM_HEAP_POINTER_NAME, 0, max_batch_size * sizeof(edge_t), DPU_XFER_DEFAULT));

        DPU_FOREACH(dpu_set, dpu, index) {
            DPU_ASSERT(dpu_copy_to(dpu, "edges_in_batch", 0, &(dpu_info_array[t * NR_DPUS + index].edge_count_batch), sizeof(dpu_info_array[t * NR_DPUS + index].edge_count_batch)));
            dpu_info_array[t * NR_DPUS + index].edge_count_batch = 0;
        }

        DPU_ASSERT(dpu_launch(dpu_set, DPU_ASYNCHRONOUS));

        pthread_mutex_unlock(mutex);
    }
}

float timedifference_msec(struct timeval t0, struct timeval t1){
    return (t1.tv_sec - t0.tv_sec) * 1000.0f + (t1.tv_usec - t0.tv_usec) / 1000.0f;
}
