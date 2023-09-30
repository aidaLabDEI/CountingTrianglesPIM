#ifndef __HOST_UTIL_H__
#define __HOST_UTIL_H__

#include <sys/time.h>
#include <dpu.h>

#include "../common/common.h"

#ifndef NR_THREADS
#define NR_THREADS 1
#endif

//Given X bytes of RAM available, X should be divided by (NR_THREADS * NR_DPUS * sizeof(edge_t))
//At the same time, it should not be more than 4194304 (32MB of edges)
#ifndef BATCH_SIZE_EDGES
#define BATCH_SIZE_EDGES 1048576
#endif

typedef struct{
    edge_t* batch;  //Pointer to the array containing the edges in the current batch for the DPU
    uint64_t edge_count_batch;  //Current number of edges in the batch for the DPU
}dpu_info_t;

typedef struct{
    uint32_t th_id;

    char* mmaped_file;  //Information about file
    uint32_t file_size;

    uint32_t from_char;  //Section of the file that the thread needs to read
    uint32_t to_char;

    dpu_arguments_t* dpu_input_arguments_ptr;
    dpu_info_t* dpu_info_array;
    struct dpu_set_t dpu_set;

    pthread_mutex_t* send_to_dpu_mutex;
} handle_edges_thread_args_t;

//Get ordered colors of the edge
edge_colors_t get_edge_colors(edge_t edge, dpu_arguments_t* dpu_input_arguments_ptr);

//Function executed by each thread handling the edges. The file is read and the edges are inserted in the correct batch
void* handle_edges_file(void* args_thread);

//Insert the current edge into the correct batches considering how triplets are assigned to the DPUs
void insert_edge_into_batches(edge_t current_edge, struct dpu_set_t dpu_set, dpu_info_t* dpu_info_array, dpu_arguments_t* dpu_input_arguments_ptr, pthread_mutex_t* send_to_dpu_mutex, uint32_t th_id);

//Send the full batch to the specific DPU. th_id_to is not included
void send_batches(uint32_t th_id_from, uint32_t th_id_to, dpu_info_t* dpu_info_array, pthread_mutex_t* mutex, struct dpu_set_t dpu_set);

//Get time difference between two moments to calculate execution time
float timedifference_msec(struct timeval t0, struct timeval t1);

#endif /* __HOST_UTIL_H_ */
