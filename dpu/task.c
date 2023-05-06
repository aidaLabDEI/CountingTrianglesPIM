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
#include "order_and_locate.h"

//Variables set by the host
__host dpu_arguments_t DPU_INPUT_ARGUMENTS;  //Frequently used values
__mram_noinit uint64_t id;  //8 bytes for CPU-DPU transfer
__mram_noinit batch_t batch;

//When the last batch is processed, this variable is set to true. In the next execution
//the DPU will start counting the triangles
bool is_graph_ended = false;

//Variable that will be read by the host at the end
__host uint64_t triangle_estimation;  //Necessary to have a variable of 8 bytes for transferring to host

//Current count of edges in the sample (limited by sample size)
uint32_t edges_in_sample = 0;

//Sample inside the MRAM heap that starts from the heap pointer. Everything in the MRAM heap will start after the sample
__mram_ptr edge_t* sample = DPU_MRAM_HEAP_POINTER;
__mram_ptr void* AFTER_SAMPLE_HEAP_POINTER;

//Stores all the triplets handled by this DPU
triplets_array_t handled_triplets = {false, 0, NULL};

//Number of edges handled by this DPU.
//Edges inside the DPU, removed in the past or not considered by chance.
//The edges not handled because of the triplets are not counted
uint32_t total_edges = 0;

//Contains the partition of the sample assigned to each tasklet in the QuickSort
tasklet_partitions_t t_partitions;

//Number of uniqe nodes in the sample
uint32_t unique_nodes = 0;

//Save the pointers for the WRAM buffers for each tasklet to make the buffers persistent through different executions
void* tasklets_buffer_ptrs[NR_TASKLETS];

//General barrier to sync all the tasklets
BARRIER_INIT(sync_tasklets, NR_TASKLETS);

//When trying to replace an edge inside the sample if it is full, it is necessary to be certain
//That all the tasklets have copied their local buffer to the sample in the MRAM.
//There is a barrier to allow for all the tasklets to transfer their buffer to the MRAM
bool is_sample_full = false;
BARRIER_INIT(sync_replace_in_sample, NR_TASKLETS);

MUTEX_INIT(insert_into_sample);  //Virtual insertion. Increase counter, but insertion is done according to buffer size
MUTEX_INIT(add_triangles);  //Each tasklets can add its triangle count to the global count

//It is not possible to use edges_in_sample to know where to save. This variable keeps track of the first
//free index in the sample where it is possible to save data from the WRAM buffers
uint32_t index_to_save_sample = 0;
MUTEX_INIT(transfer_to_sample);  //Real transfer to the sample in the MRAM

int main() {

    if(!handled_triplets.is_set){

        //Make only one tasklet set up the variables for the entire DPU (and so all tasklets)
        if(me() == 0){
            initial_setup(id, &handled_triplets, &DPU_INPUT_ARGUMENTS);  //Set random number seed and determine triplets

            //Calculate the new heap pointer that points to the address after the sample
            AFTER_SAMPLE_HEAP_POINTER = DPU_MRAM_HEAP_POINTER + sizeof(edge_t) * DPU_INPUT_ARGUMENTS.SAMPLE_SIZE;
            assert(WRAM_BUFFER_SIZE % 16 == 0);  //8 bytes aligned, but structs of 16 bytes use this buffer
            assert(WRAM_BUFFER_SIZE >= 64);  //Useless smaller than this (only one node_loc_t would be transferred each time)
        }

        barrier_wait(&sync_tasklets);  //Wait for the setup to happen
        tasklets_buffer_ptrs[me()] = mem_alloc(WRAM_BUFFER_SIZE);  //Create the buffer in the WRAM for every tasklet. Generic void* pointer
    }

    if(handled_triplets.size == 0){  //Do nothing if DPU does not handle any triplet
        return 0;
    }

    //Locate the buffer in the WRAM for this tasklet each run
    void* wram_buffer_ptr = tasklets_buffer_ptrs[me()];

    //Range handled by a tasklet
    uint32_t handled_edges = (uint32_t)batch.size/NR_TASKLETS;
    uint32_t edge_count_batch_local = handled_edges * me();  //The first edge handled by a tasklet
    uint32_t edge_count_batch_to;  //Not included, limits the edges in the batch handled by a tasklet
    if(me() != NR_TASKLETS-1){
         edge_count_batch_to = handled_edges * (me()+1);
    }else{
        edge_count_batch_to = batch.size;  //If last tasklet, handle all remaining edges
    }

    //It is more efficient to read some data from the MRAM to the WRAM (and write from the WRAM to MRAM)
    //Less single accesses to the MRAM, but bigger ones
    uint32_t edges_in_wram_cache = (WRAM_BUFFER_SIZE / sizeof(edge_t)) >> 1 ;  //Divide the buffer in two (with right shift)

    //Half buffer will contain edges from the batch, half will contain edges to send to the sample
    edge_t* batch_buffer_wram = (edge_t*) wram_buffer_ptr;
    edge_t* sample_buffer_wram = (edge_t*) wram_buffer_ptr + edges_in_wram_cache;

    uint32_t batch_buffer_index = edges_in_wram_cache;  //In this way, the batch cache is filled at the first iteration
    uint32_t sample_buffer_index = 0;

    while(edge_count_batch_local < edge_count_batch_to){  //Until the end of the section of the batch is reached

        //Transfer some edges of the batch to the WRAM
        if(batch_buffer_index == edges_in_wram_cache){
            //It is necessary to read only the edges assigned to the tasklet, so only the needed edges, until
            //the maximum allowed, are read
            uint32_t edges_to_read;
            if((edge_count_batch_to - edge_count_batch_local) >= edges_in_wram_cache){
                edges_to_read = edges_in_wram_cache;
            }else{
                edges_to_read = edge_count_batch_to - edge_count_batch_local;
            }

            read_from_mram(&batch.edges_batch[edge_count_batch_local], batch_buffer_wram, edges_to_read * sizeof(edge_t));

            batch_buffer_index = 0;
        }

        edge_t current_edge = batch_buffer_wram[batch_buffer_index];
        batch_buffer_index++;

        assert(current_edge.u <= current_edge.v);  //It is necessary for the nodes to be ordered inside the edge
        edge_count_batch_local++;

        edge_colors_t current_edge_colors = get_edge_colors(current_edge, &DPU_INPUT_ARGUMENTS);

        //Skip edge if it is not handled by this DPU
        if(!is_edge_handled(current_edge_colors, &handled_triplets)){
            continue;
        }

        mutex_lock(insert_into_sample);  //Only one tasklet at the time can "virtually" modify the sample

        //Count of edges assigned to this DPU
        total_edges++;

         //Add to sample if |S| < M
        if(edges_in_sample < DPU_INPUT_ARGUMENTS.SAMPLE_SIZE){
            //write the edge in the buffer in the WRAM
            sample_buffer_wram[sample_buffer_index] = current_edge;
            sample_buffer_index++;
            edges_in_sample++;

            mutex_unlock(insert_into_sample);
        }else{  //Sample is full
            mutex_unlock(insert_into_sample);  //Unlock the mutex, while potentially transferring the buffer to the MRAM

            //Transfer the last edges to the mram if the sample is full (the sample buffer becomes useless from now on)
            if(sample_buffer_index != 0){
                mutex_lock(transfer_to_sample);

                write_to_mram(sample_buffer_wram, &sample[index_to_save_sample], sample_buffer_index * sizeof(edge_t));
                index_to_save_sample += sample_buffer_index;
                sample_buffer_index = 0;

                mutex_unlock(transfer_to_sample);
            }

            //Biased dice roll
            float u_rand = (float)rand() / ((float)ULONG_MAX+1.0);
    		double thres = ((float)DPU_INPUT_ARGUMENTS.SAMPLE_SIZE)/total_edges;

            //Randomly decide if replace or not an edge in the sample
            if (u_rand < thres){

                //Wait for all the tasklets to transfer their WRAM buffer in the MRAM before replacing edges
                if(!is_sample_full){
                    barrier_wait(&sync_replace_in_sample);
                    is_sample_full = true;  //No real problem if concurrent write to this variable
                }
                mutex_lock(insert_into_sample);

                uint32_t random_index = rand_range(0, DPU_INPUT_ARGUMENTS.SAMPLE_SIZE-1);
                sample[random_index] = current_edge;  //Random access. No benefit in using WRAM cache

                mutex_unlock(insert_into_sample);
            }
        }

        //Transfer the current sample buffer to the MRAM if full
        if(sample_buffer_index == edges_in_wram_cache){
            mutex_lock(transfer_to_sample);

            write_to_mram(sample_buffer_wram, &sample[index_to_save_sample], edges_in_wram_cache * sizeof(edge_t));
            index_to_save_sample += edges_in_wram_cache;
            sample_buffer_index = 0;

            mutex_unlock(transfer_to_sample);
        }
    }

    //Transfer the last edges to the sample
    if(sample_buffer_index != 0){
        mutex_lock(transfer_to_sample);

        write_to_mram(sample_buffer_wram, &sample[index_to_save_sample], sample_buffer_index * sizeof(edge_t));
        index_to_save_sample += sample_buffer_index;
        sample_buffer_index = 0;

        mutex_unlock(transfer_to_sample);
    }

    //Wait for all the tasklets to transfer their WRAM buffer in the MRAM before allowing edge replacement for other tasklets
    if(!is_sample_full){
        barrier_wait(&sync_replace_in_sample);
        is_sample_full = true;  //No real problem if concurrent write to this variable
    }

    barrier_wait(&sync_tasklets);  //Sync finishing handling batch

    //If the last batch was the last and the sample is not empty, start counting
    if(is_graph_ended && edges_in_sample > 0){

        if(me() == 0){  //One tasklet creates the partitions for the QuickSort for all tasklets
            t_partitions.partitions = mem_alloc(NR_TASKLETS * sizeof(partition_t));
            t_partitions.size = 0;
            tasklet_partition(sample, 0, edges_in_sample-1, NR_TASKLETS, &t_partitions);
        }

        barrier_wait(&sync_tasklets);  //Wait for the partitions to be created

        //If there is a partition for a given tasklet. Some tasklets may not have a partition if the sample is small
        if(me() < t_partitions.size){
            sort_sample(sample, t_partitions.partitions[me()].start, t_partitions.partitions[me()].end);
        }

        barrier_wait(&sync_tasklets);  //Wait for the sort to happen

        if(me() == 0){  //One tasklets finds the locations of the nodes
            unique_nodes = node_locations(sample, edges_in_sample, AFTER_SAMPLE_HEAP_POINTER, wram_buffer_ptr);
        }

        barrier_wait(&sync_tasklets);  //Wait for the location finding

        //Divide triangle counting between tasklets
        uint32_t edges_per_tasklet = (uint32_t)edges_in_sample/NR_TASKLETS;
        uint32_t from_edge_sample = edges_per_tasklet * me();  //The first edge in the sample handled by a tasklet
        uint32_t to_edge_sample;  //Not included

        if(me() != NR_TASKLETS-1){
             to_edge_sample = edges_per_tasklet * (me()+1);
        }else{
            to_edge_sample = edges_in_sample;  //If last tasklet, handle all remaining edges in sample
        }

        int32_t local_triangle_estimation = count_triangles(sample, from_edge_sample, to_edge_sample, &handled_triplets, unique_nodes, AFTER_SAMPLE_HEAP_POINTER, wram_buffer_ptr, &DPU_INPUT_ARGUMENTS);

        //Add the triangles counted by each tasklet
        mutex_lock(add_triangles);
        triangle_estimation += local_triangle_estimation;
        mutex_unlock(add_triangles);

        //Wait for all tasklets to count their triangles
        barrier_wait(&sync_tasklets);

        if(me() == 0){
            //Normalization of the result considering the substituted edges may have removed triangles
            float p = ((float)DPU_INPUT_ARGUMENTS.SAMPLE_SIZE/total_edges)*((float)(DPU_INPUT_ARGUMENTS.SAMPLE_SIZE-1)/(total_edges-1))*((float)(DPU_INPUT_ARGUMENTS.SAMPLE_SIZE-2)/(total_edges-2));
            p = p < 1.0 ? p : 1.0;
            triangle_estimation = (uint64_t) triangle_estimation/p;
        }
    }

    //If there is not this barrier wait, the graph is considered ended and a slow tasklet may current_edge
    //the counting portion before it is needed
    barrier_wait(&sync_tasklets);

    //This is the last batch. In the next execution triangles will be counted and it is necessary to skip the while loop
    if(batch.size < EDGES_IN_BATCH){
        batch.size = 0;
        is_graph_ended = true;
    }

    return 0;
}
