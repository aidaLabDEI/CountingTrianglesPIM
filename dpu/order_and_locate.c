#include <attributes.h>  //For __mram_ptr
#include <stdint.h>  //Fixed size integers
#include <mram.h>  //Transfer data between WRAM and MRAM. Access MRAM
#include <assert.h>  //Assert
#include <stdlib.h>  //Various things
#include <stdio.h>  //Mainly debug messages
#include <stdbool.h>  //Booleans

#include "../common/common.h"
#include "order_and_locate.h"
#include "dpu_util.h"

//end is included
void sort_sample(__mram_ptr edge_t* sample, uint32_t start, uint32_t end) {
    assert(sample != NULL);

    //Contain the boundaries of the simulated recursion levels
    //With 32 indexes, 2^32 elements can be ordered
    int32_t max_levels = 32;
    uint32_t level_start[32];
    uint32_t level_end[32];

    uint32_t local_start, local_end;  //Indexes of the elements inside each simulated recursion
    int32_t i = 0;  //Simulated recursion level (can be negative)

    level_start[0] = start;  //First simulated recursion spans across all assigned section of the sample
    level_end[0] = end;

    while (i >= 0) {  ///While there are still some levels to handle

        if(i == max_levels - 1){  //If last available level is reached, the array cannot be ordered
            assert(false);
        }

        local_start = level_start[i];  //local start at level i
        local_end = level_end[i];  //local end at level i

        if (local_start < local_end) {

            //Partition the the array
            uint32_t p = partition(sample, local_start, local_end);

            //The next level will sort the left section
            level_start[i+1] = local_start;
            level_end[i+1] = p;

            //Overwritten the current level. It will sort the right section
            level_start[i] = p+1;

            i++;  //Increse one level

            //If this level(after i++) is bigger than the previous one, swap them (execute before the smaller one. Tail "recursion")
            //It is guaranteed that 2^(max_levels) elements can be ordered
            if(level_end[i] - level_start[i] > level_end[i-1] - level_start[i-1]){
                uint32_t temp = level_start[i];
                level_start[i] = level_start[i-1];
                level_start[i-1] = temp;

                temp = level_end[i];
                level_end[i] = level_end[i-1];
                level_end[i-1] = temp;
            }

        } else {
            //If the level does not need to be ordered, order the level i-1
            i--;
        }
    }
}

//Hoare partitioning
uint32_t partition(__mram_ptr edge_t* array, uint32_t start, uint32_t end) {
    assert(array != NULL);

    uint32_t pivot_index = rand_range(start, end-1);  //Pivot choosen at random (end cannot be used with Hoare partition)
    edge_t pivot = array[pivot_index];

    int32_t i = start - 1;
    int32_t j = end + 1;

    //Works well because there are no repeated edges (no edge can be equal to the pivot)
    while (true) {
        do{
            i++;
        }while((array[i].u < pivot.u) || (array[i].u == pivot.u && array[i].v < pivot.v));

        do{
            j--;
        }while((array[j].u > pivot.u) || (array[j].u == pivot.u && array[j].v > pivot.v));

        if(i>=j){
            return j;
        }

        //Swap array[i] and array[j]
        edge_t temp = array[i];
        array[i] = array[j];
        array[j]= temp;
    }
}

void tasklet_partition(__mram_ptr edge_t* sample, uint32_t start, uint32_t end, uint32_t nr_tasklets, tasklet_partitions_t* t_partitions_ptr){
    assert(sample != NULL);
    assert(nr_tasklets <= NR_TASKLETS);
    assert(t_partitions_ptr != NULL);

    if(start < end && nr_tasklets > 1){  //If it is possible to create other partitions between start and and
        uint32_t p = partition(sample, start, end);  //Partition the section between start and end

        uint32_t edges = end-start+1;  //Edges between start and end
        uint32_t edges_first_partition = p-start;  //Edges inside the first half

        //Divide the number of tasklets to handle a partition according to the number of edges in that partition
        //Because p is decided randomly, there may be two sections of very different size and equal division
        //Of tasklets would not be ideal
        uint32_t tasklet_first_section = (edges_first_partition* nr_tasklets / edges);

        //At least one tasklet per section
        if(tasklet_first_section == 0){
            tasklet_first_section++;
        }

        if(tasklet_first_section == nr_tasklets){
            tasklet_first_section--;
        }

        tasklet_partition(sample, start, p, tasklet_first_section, t_partitions_ptr);
        tasklet_partition(sample, p+1, end, nr_tasklets-tasklet_first_section, t_partitions_ptr);

    }else{  //Not possible to divide more, save the current partition
        t_partitions_ptr->partitions[t_partitions_ptr->size].start = start;
        t_partitions_ptr->partitions[t_partitions_ptr->size].end = end;

        t_partitions_ptr->size += 1;
    }
}

//end is excluded (different from QuickSort)
bool is_ordered_sample(__mram_ptr edge_t* sample, uint32_t start, uint32_t end){
    assert(sample != NULL);
    assert(start < end);

    for(uint32_t i = start+1; i < end; i++){
        if(sample[i-1].u > sample[i].u){
            return false;
        }

        if(sample[i-1].u == sample[i].u && sample[i-1].v > sample[i].v){
            return false;
        }
    }

    return true;
}

uint32_t node_locations(__mram_ptr edge_t* sample, uint32_t edges_in_sample, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER, void* wram_buffer_ptr){
    assert(sample != NULL);
    assert(AFTER_SAMPLE_HEAP_POINTER != NULL);
    assert(wram_buffer_ptr != NULL);

    uint32_t index = 0;  //Index for traversing the sample

    //Used only the first node of each edge
    uint32_t current_node_id;
    uint32_t index_in_sample;  //Index of the first occurrence of the unique node inside tha sample
    uint32_t number_bigger_neighbors;  //The number of neighbors of the current node. Only the neighbors with bigger ids are considered

    uint32_t mram_heap_offset = 0;  //Offset for the location elements inside the heap, starting from after the space occupied by the sample

    uint32_t unique_nodes = 0;  //Number of unique nodes found

    //Create a buffer in the WRAM of the sample to speed up research
    uint32_t edges_in_wram_cache = (WRAM_BUFFER_SIZE / sizeof(edge_t)) >> 1;  //Divide by 2 with right shift
    //Use half the WRAM buffer for buffering the sample
    edge_t* sample_buffer_wram = (edge_t*) wram_buffer_ptr;

    //Save the nodes info inside the WRAM to make a bigger transfer
    uint32_t nodes_loc_in_wram_cache = (WRAM_BUFFER_SIZE / sizeof(node_loc_t)) >> 1;  //Divide by 2 with right shift
    //Use the other half of the WRAM buffer to store node locations that will be transferred to the MRAM
    node_loc_t* nodes_loc_buffer_wram = (node_loc_t*) wram_buffer_ptr + nodes_loc_in_wram_cache;

    //Indexes to traverse the WRAM buffers
    uint32_t nodes_loc_buffer_index = 0;
    uint32_t sample_buffer_index = edges_in_wram_cache;

    //Search all the unique nodes considering only the first nodes of the edges
    while(index < edges_in_sample){

        //Transfer some edges of the sample to the WRAM
        if(sample_buffer_index == edges_in_wram_cache){
            //If edges_in_sample is not a multiple of edges_in_wram_cache edges, some bytes will be read from MRAM
            //Containing useless informations. This is not a problem because the loop will stop with certain indexes
            read_from_mram(&sample[index], sample_buffer_wram, edges_in_wram_cache * sizeof(edge_t));
            sample_buffer_index = 0;
        }

        //Initialise the variables for this unique node
        number_bigger_neighbors = 0;
        current_node_id = sample_buffer_wram[sample_buffer_index].u;
        index_in_sample = index;
        unique_nodes++;

        //Count the number of nodes that have the current_node as value u in the edge
        //The result is the number of neighbors of that node
        //This is possible because the sample is ordered, there are no duplicates and u<v in each edge
        while(sample_buffer_wram[sample_buffer_index].u == current_node_id){

            number_bigger_neighbors++;
            index++;  //Index in sample
            sample_buffer_index++;

            ///Transfer some edges of the sample to the WRAM
            if(sample_buffer_index == edges_in_wram_cache){
                //If edges_in_sample is not a multiple of edges_in_wram_cache edges, some bytes will be read from MRAM
                //Containing useless informations. This is not a problem because the loop will stop with certain indexes
                read_from_mram(&sample[index], sample_buffer_wram, edges_in_wram_cache * sizeof(edge_t));
                sample_buffer_index = 0;
            }
        }

        //Transfer the data to the mram
        node_loc_t current_node_info = (node_loc_t) {current_node_id, index_in_sample, number_bigger_neighbors, 0};  //0 for padding to 16bytes

        nodes_loc_buffer_wram[nodes_loc_buffer_index] = current_node_info;
        nodes_loc_buffer_index++;

        //Transfer the nodes info to the MRAM if the location buffer is full
        if(nodes_loc_buffer_index == nodes_loc_in_wram_cache){
            write_to_mram(nodes_loc_buffer_wram, AFTER_SAMPLE_HEAP_POINTER + mram_heap_offset, nodes_loc_in_wram_cache * sizeof(node_loc_t));
            mram_heap_offset += nodes_loc_in_wram_cache * sizeof(node_loc_t);
            nodes_loc_buffer_index = 0;
        }
    }

    //Transfer the last node infos
    if(nodes_loc_buffer_index != 0){
        write_to_mram(nodes_loc_buffer_wram, AFTER_SAMPLE_HEAP_POINTER + mram_heap_offset, nodes_loc_in_wram_cache * sizeof(node_loc_t));
    }

    return unique_nodes;
}

//No need to use WRAM buffer because it is only for debugging
void print_node_locations(uint32_t number_of_nodes, __mram_ptr void* AFTER_SAMPLE_HEAP_POINTER){
    assert(AFTER_SAMPLE_HEAP_POINTER != NULL);

    printf("Printing the node location informations. There are %d unique nodes:\n", number_of_nodes);

    for(uint32_t i = 0; i < number_of_nodes; i++){
        node_loc_t current_node;
        mram_read(AFTER_SAMPLE_HEAP_POINTER + i * sizeof(node_loc_t), &current_node, sizeof(node_loc_t));  //Read the informations of one node from MRAM
        printf("Id: %d Index in sample: %d Num bigger neighbors: %d\n", current_node.id, current_node.index_in_sample, current_node.number_bigger_neighbors);
    }
}
