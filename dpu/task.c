#include <mram.h>  //Transfer data between WRAM and MRAM. Access MRAM
#include <stdint.h>  //Fixed size integers
#include <stdlib.h>  //Various things
#include <limits.h>  //Max values (used for biased dice roll)
#include <stdio.h>  //Mainly debug messages
#include <stdbool.h>  //Booleans
#include <alloc.h>  //Alloc heap in WRAM
#include <assert.h> //Assert
#include <defs.h>  //Get tasklet id
#include <barrier.h>  //Barrier for tasklets
#include <mutex.h>  //Mutex for tasklets

#include "../common/common.h"
#include "dpu_util.h"
#include "triangle_counter.h"
#include "quicksort.h"
#include "locate_nodes.h"

//Variables set by the host
__host dpu_arguments_t DPU_INPUT_ARGUMENTS;

//When the execution code is odd, that means that the graph has ended,
//the host has sent the value and the triangle counting can start
__host execution_config_t execution_config = {0, 0};

//Variable that will be read by the host at the end
__host uint64_t triangle_estimation;

//Current count of edges in the sample (limited by sample size)
uint32_t edges_in_sample = 0;

__mram_ptr edge_t* batch;  //Max size for batch is 30MB
__host uint64_t edges_in_batch;

__mram_ptr edge_t* sample;
__mram_ptr void* FREE_SPACE_HEAP_POINTER;  //The address space where there isn't the sample (beginning or middle of the heap)

//Transfer the data first to the MRAM, and then to the WRAM.
//This to allow the WRAM buffer to be allocated dynamically
__mram_ptr node_frequency_t* top_frequent_nodes_MRAM;  //No interference with other elements in MRAM
node_frequency_t* top_frequent_nodes;
__host uint64_t nr_top_nodes;

//The setup happens only once
bool is_setup_done = false;

//Number of edges assigned to this DPU.
uint32_t total_edges = 0;

//Save the pointers for the WRAM buffers for each tasklet to make the buffers persistent through different executions
void* tasklets_buffer_ptrs[NR_TASKLETS];

//Save results of different tasklets to allow for tree-based reduction
//64 bits because sums may overflow 32-bit integers
uint64_t messages[NR_TASKLETS];

//General barrier to sync all the tasklets
BARRIER_INIT(sync_tasklets, NR_TASKLETS);

//When trying to replace an edge inside the sample if it is full, it is necessary to be certain
//That all the tasklets have copied their local buffer to the sample in the MRAM.
//There is a barrier to allow for all the tasklets to transfer their buffer to the MRAM
bool is_sample_full = false;
BARRIER_INIT(sync_replace_in_sample, NR_TASKLETS);

MUTEX_INIT(insert_into_sample);  //Virtual insertion

//It is not possible to use edges_in_sample to know where to save. This variable keeps track of the first
//free index in the sample where it is possible to save data from the WRAM buffers
uint32_t global_index_to_save_sample = 0;
MUTEX_INIT(transfer_to_sample);  //Real transfer to the sample in the MRAM
MUTEX_INIT(replace_in_sample);

int main() {

    if(!is_setup_done){

        //Make only one tasklet set up the variables for the entire DPU (and so all tasklets)
        if(me() == 0){
            mem_reset();  //Reset WRAM heap before starting
            srand(DPU_INPUT_ARGUMENTS.seed);  //Effect is global

            //If Misra-Gries is used
            if(DPU_INPUT_ARGUMENTS.t != 0){
                top_frequent_nodes = mem_alloc(DPU_INPUT_ARGUMENTS.t * sizeof(node_frequency_t));
            }
        }
        barrier_wait(&sync_tasklets);  //Wait for memory reset
        tasklets_buffer_ptrs[me()] = mem_alloc(WRAM_BUFFER_SIZE);  //Create the buffer in the WRAM for every tasklet. Generic void* pointer

        is_setup_done = true;
        return 0;
    }

    //Get the update index and see if its even or odd
    //Pointers need to be updated because the sorting moves the sample
    if(((execution_config.execution_code >> 1) & 1) == 0){
        batch = DPU_MRAM_HEAP_POINTER;
        sample = DPU_MRAM_HEAP_POINTER + 32 * 1024 * 1024;
        top_frequent_nodes_MRAM = DPU_MRAM_HEAP_POINTER;
    }else{
        batch = DPU_MRAM_HEAP_POINTER + 32 * 1024 * 1024;
        sample = DPU_MRAM_HEAP_POINTER;
        top_frequent_nodes_MRAM = DPU_MRAM_HEAP_POINTER + 32 * 1024 * 1024;
    }

    //Locate the buffer in the WRAM for this tasklet each run
    void* wram_buffer_ptr = tasklets_buffer_ptrs[me()];

    if((execution_config.execution_code & 1) == 0){  //SAMPLE CREATION OPERATIONS

        //Range handled by a tasklet
        uint32_t handled_edges = (uint32_t)edges_in_batch/NR_TASKLETS;
        uint32_t batch_index_local = handled_edges * me();  //The first edge handled by a tasklet
        uint32_t batch_index_to = (me() == NR_TASKLETS-1) ? edges_in_batch : handled_edges * (me()+1);

        edge_t* batch_buffer = (edge_t*) wram_buffer_ptr;
        uint32_t max_edges_in_batch_buffer = (WRAM_BUFFER_SIZE / sizeof(edge_t));

        uint32_t batch_buffer_index = max_edges_in_batch_buffer;  //Allows for data to be transfered the first iteration

        //This is used to indicate to the single tasklet where to save its buffer in the sample, without requiring to have the transfer inside the mutex
        //It is determined considering the variable global_index_to_save_sample
        uint32_t local_index_to_save_sample = 0;

        while(batch_index_local < batch_index_to){  //Until the end of the section of the batch is reached

            //Transfer some edges of the batch to the WRAM
            uint32_t edges_in_batch_buffer = 0;
            if(batch_buffer_index == max_edges_in_batch_buffer){
                //It is necessary to read only the edges assigned to the tasklet.
                if((batch_index_to - batch_index_local) >= max_edges_in_batch_buffer){
                    edges_in_batch_buffer = max_edges_in_batch_buffer;
                }else{
                    edges_in_batch_buffer = batch_index_to - batch_index_local;
                }

                mram_read(&batch[batch_index_local], batch_buffer, edges_in_batch_buffer * sizeof(edge_t));

                batch_buffer_index = 0;
            }

            if(!is_sample_full){
                mutex_lock(insert_into_sample);  //Only one tasklet at the time can modify the sample

                //If there is enough space for some edges in the current tasklet batch buffer, copy them to the sample
                if(DPU_INPUT_ARGUMENTS.sample_size - edges_in_sample > 0){

                    uint32_t edges_to_copy;
                    if(DPU_INPUT_ARGUMENTS.sample_size - edges_in_sample >= edges_in_batch_buffer){
                        edges_to_copy = edges_in_batch_buffer;
                    }else{
                        edges_to_copy = DPU_INPUT_ARGUMENTS.sample_size - edges_in_sample;  //Greater than zero because sample is not full
                    }

                    edges_in_sample += edges_to_copy;
                    total_edges += edges_to_copy;

                    local_index_to_save_sample = global_index_to_save_sample;
                    global_index_to_save_sample += edges_to_copy;

                    mutex_unlock(insert_into_sample);

                    mram_write(batch_buffer, &sample[local_index_to_save_sample], edges_to_copy * sizeof(edge_t));

                    if(edges_to_copy == edges_in_batch_buffer){  //All edges are already transferred. Get new edges
                        batch_buffer_index = max_edges_in_batch_buffer;
                        batch_index_local += max_edges_in_batch_buffer;
                        continue;
                    }

                    batch_buffer_index = edges_to_copy;  //There are still edges to consider
                }else{
                    mutex_unlock(insert_into_sample);
                }

                //If a tasklet reaches this point it means that the sample is now full
                //It is necessary to wait for all tasklets to copy their edges (if any) in the sample
                //This is necessary to be sure that all data is transfered before starting to do replacements
                barrier_wait(&sync_replace_in_sample);
                is_sample_full = true;
            }

            ////There are still some edges to consider, and the sample is full. So edge replacement is necessary////

            edge_t current_edge = batch_buffer[batch_buffer_index];
            batch_buffer_index++;

            batch_index_local++;

            mutex_lock(replace_in_sample);
            total_edges++;
            mutex_unlock(replace_in_sample);

            //Biased dice roll
            float u_rand = (float)rand() / ((float)UINT_MAX+1.0);  //UINT_MAX is the maximum value that can be returned by rand()
    		float thres = ((float)DPU_INPUT_ARGUMENTS.sample_size)/total_edges;

            //Randomly decide if to replace or not an edge in the sample
            if (u_rand < thres){
                uint32_t random_index = rand_range(0, DPU_INPUT_ARGUMENTS.sample_size-1);
                //Tasklet-safe
                mram_write(&current_edge, &sample[random_index], sizeof(current_edge));  //Random access. No benefit in using WRAM cache
            }
        }

        if(!is_sample_full){  //Unlock possible tasklets waiting for all the tasklets to copy to the sample
            barrier_wait(&sync_replace_in_sample);
        }

    }else if(edges_in_sample > 0){  //TRIANGLE COUNTING OPERATIONS
        uint32_t tasklet_id = me();  //Makes it easier to understand the code

        //If Misra-Gries is used
        if(DPU_INPUT_ARGUMENTS.t != 0){

            //Split the workload equally among the tasklets
            uint32_t edges_per_tasklet = edges_in_sample/NR_TASKLETS;
            uint32_t from_edge = edges_per_tasklet * tasklet_id;
            uint32_t to_edge = (tasklet_id == NR_TASKLETS-1) ? edges_in_sample : edges_per_tasklet * (tasklet_id+1);

            //Transfer the most frequent nodes from the MRAM to the WRAM only during the first update
            if(execution_config.execution_code == 1 && tasklet_id == 0){
                mram_read(top_frequent_nodes_MRAM, top_frequent_nodes, DPU_INPUT_ARGUMENTS.t * sizeof(node_frequency_t));
            }
            barrier_wait(&sync_tasklets);

            frequent_nodes_remapping(sample, from_edge, to_edge, wram_buffer_ptr, nr_top_nodes, top_frequent_nodes, execution_config.max_node_id);
            barrier_wait(&sync_tasklets);
        }

        // Move data from the current position of the sample, overwriting the space used by the batch
        sort_sample(edges_in_sample, sample, batch, wram_buffer_ptr, execution_config.max_node_id);
        barrier_wait(&sync_tasklets);  //Wait for the sort to happen

        //After the quicksort, some pointers change
        if(tasklet_id == 0){
            FREE_SPACE_HEAP_POINTER = (__mram_ptr void*) sample;  //Now the memory space where the unordered sample can be ovewritten
            sample = (__mram_ptr void*) batch;  //Now the sample is placed where the batch was
        }
        barrier_wait(&sync_tasklets);

        //Each message will contain the local_unique_nodes
        messages[tasklet_id] = node_locations(sample, edges_in_sample, FREE_SPACE_HEAP_POINTER, wram_buffer_ptr);

        //Tree-based reduction to find the number of unique nodes
        barrier_wait(&sync_tasklets);

        #pragma unroll
        for (uint32_t offset = 1; offset < NR_TASKLETS; offset <<= 1){
            if((tasklet_id & ((offset<<1) - 1)) == 0){
                //Add up the number of local unique nodes
                messages[tasklet_id] += messages[tasklet_id + offset];
            }
            barrier_wait(&sync_tasklets);
        }

        //The first tasklet message will contain the number of unique nodes
        messages[tasklet_id] = count_triangles(sample, edges_in_sample, messages[0], FREE_SPACE_HEAP_POINTER, wram_buffer_ptr);

        //Tree-based reduction to find the total number of triangles
        barrier_wait(&sync_tasklets);

        #pragma unroll
        for (uint32_t offset = 1; offset < NR_TASKLETS; offset <<= 1){
            if((tasklet_id & ((offset<<1) - 1)) == 0){
                //Add up the number of local unique nodes
                messages[tasklet_id] += messages[tasklet_id + offset];
            }
            barrier_wait(&sync_tasklets);
        }

        if(me() == 0){
            if(edges_in_sample < total_edges){
                //Normalization of the result considering the substituted edges may have removed triangles
                double p = ((float)DPU_INPUT_ARGUMENTS.sample_size/total_edges)*((float)(DPU_INPUT_ARGUMENTS.sample_size-1)/(total_edges-1))*((float)(DPU_INPUT_ARGUMENTS.sample_size-2)/(total_edges-2));

                //The first tasklet message will contain the number of triangles counted by the tasklets
                triangle_estimation = (uint64_t) messages[0]/p;
            }else{
                triangle_estimation = messages[0];
            }
        }
    }

    return 0;
}
