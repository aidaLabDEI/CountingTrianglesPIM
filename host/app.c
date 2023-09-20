#include <assert.h>  //Assert
#include <dpu.h>  //Create dpu set
#include <dpu_log.h>  //Get logs from dpus (used for debug)
#include <stdio.h>  //Open file and print
#include <time.h>  //Random seed
#include <sys/time.h>  //Measure execution time
#include <stdlib.h>  //Various
#include <stdint.h>  //Known size integers
#include <stdbool.h>  //Booleans
#include <math.h> //Round

#include "../common/common.h"

#ifndef DPU_BINARY
#define DPU_BINARY "./task"
#endif

//If TESTING = 1, the output numbers have no description. Used to make easier to parse output when testing
#define TESTING 0

void send_batch(struct dpu_set_t dpu_set, batch_t* batch);

//Get time difference between two moments to calculate execution time
float timedifference_msec(struct timeval t0, struct timeval t1);

int main(int argc, char* argv[]){
    struct timeval start;
    gettimeofday(&start, 0);

    /*ARGUMENTS HANDLING*/
    if(argc < 5){  //First argument is executable
        printf("Invalid number of arguments. Please insert the seed for the random number generator (0 for random seed), ");
        printf("the size of the sample inside the DPUs (number of edges) (0 for max allowed value), the number of colors to use and ");
        printf("the file name containing all the edges.\n");
        return 1;
    }

    uint32_t random_seed = atoi(argv[1]);

    if(random_seed == 0){  //Set random seed that depends on time
        srand(time(NULL));
        random_seed = rand();
    }else{
        srand(random_seed);  //Set given seed
    }

    uint32_t sample_size = atoi(argv[2]);  //Size of the sample (number of edges) inside the DPUs

    if(sample_size == 0){
        sample_size = MAX_SAMPLE_SIZE;
    }
    assert(sample_size <= MAX_SAMPLE_SIZE);

     //There is no allocated space for the batch, everything is handled in the MRAM heap
     //There must be space for the sample and for the batch at the same time
    assert(EDGES_IN_BATCH * sizeof(edge_t) < (64*1024*1024 - sample_size * sizeof(edge_t)));

    uint32_t color_number = atoi(argv[3]);  //Number of colors used to color the nodes

    //Number of triplets created given the colors. binom(c+2, 3). Max one triplet per DPU
    uint32_t triplets_created = round((1.0/6) * color_number * (color_number+1) * (color_number+2));
    if(triplets_created > NR_DPUS){
        printf("More triplets than DPUs. Use more DPUs or less colors. Given %d colors, no less than %d DPUs can be used\n", color_number, triplets_created);
        return 1;
    }

    char* file_name = argv[4];

    //Determine the parameters used to color the nodes
    uint32_t hash_parameter_p = 8191;  //Prime number
    uint32_t hash_parameter_a = rand() % (hash_parameter_p-1) + 1; // a in [1, p-1]
    uint32_t hash_parameter_b = rand() % hash_parameter_p; // b in [0, p-1]

    /*INITIALIZING DPUs*/
    struct dpu_set_t dpu_set, dpu;

    DPU_ASSERT(dpu_alloc(NR_DPUS, NULL, &dpu_set));
    DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY, NULL));
    /*SENDING THE SAME STARTING ARGUMENTS TO ALL DPUs*/
    dpu_arguments_t input_arguments = {random_seed, sample_size, color_number, hash_parameter_p, hash_parameter_a, hash_parameter_b};
    DPU_ASSERT(dpu_broadcast_to(dpu_set, "DPU_INPUT_ARGUMENTS", 0, &input_arguments, sizeof(dpu_arguments_t), DPU_XFER_DEFAULT));

    /*SENDING IDS TO THE DPUS*/

    //Necessary to create an array of ids because each dpu xfer is associated with
    //an address and to send different values, each address should be different from the others.
    //Using 8 bytes ids for alignment
    uint64_t ids[NR_DPUS];  //8bytes for transfer to the DPUs
    for(uint64_t i = 0; i < NR_DPUS; i++){
        ids[i] = i;
    }
    uint32_t index;
    DPU_FOREACH(dpu_set, dpu, index) {  //dpu is a set itself
        DPU_ASSERT(dpu_prepare_xfer(dpu, &ids[index]));  //Associate an address to each dpu
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "id", 0, sizeof(ids[0]), DPU_XFER_DEFAULT));

    /*READING FILE AND SENDING BATCHES*/

    FILE * file_ptr = fopen(file_name, "r");
    assert(file_ptr != NULL);

     //buffer to read each line. Each node can use 10 chars each at max(unsigned integers of 4 bytes)
    char char_buffer[32];
    uint32_t node1, node2;

    edge_t current_edge;
    uint32_t edge_count_batch = 0;

    //Contains the edges that need to be transferred to the dpus in a broadcast
    //Dynamic memory allocation because batch may not fit inside program stack
    batch_t* batch = (batch_t*) malloc(sizeof(batch_t));

    struct timeval now;
    gettimeofday(&now, 0);
    float setup_time = timedifference_msec(start, now);
    gettimeofday(&start, 0);

    while (fgets(char_buffer, sizeof(char_buffer), file_ptr) != NULL) {  //Reads until EOF

        //Each edge is formed by two unsigned integers separated by a space
        sscanf(char_buffer, "%d %d", &node1, &node2);
        if(node1 == node2){  //Ignore edges made with two equal nodes
            continue;
        }

        if(node1 < node2){  //Nodes in edge need to be ordered
            current_edge = (edge_t){node1, node2};
        }else{
            current_edge = (edge_t){node2, node1};
        }

        batch->edges_batch[edge_count_batch] = current_edge;

        edge_count_batch++;

        //Enough edges inserted inside this batch, time to send it
        if(edge_count_batch == EDGES_IN_BATCH){
            batch->size = edge_count_batch;
            send_batch(dpu_set, batch);
            edge_count_batch = 0;  //Reset count
        }
    }

    //The file ended, sending the last batch->
    //If the previous batch contained exactly all the remaining edges,
    //A batch with only (0 0)s is sent.
    assert(edge_count_batch < EDGES_IN_BATCH);

    batch->size = edge_count_batch;

    send_batch(dpu_set, batch);
    DPU_ASSERT(dpu_sync(dpu_set));

    //Batch is not used anymore
    free(batch);

    gettimeofday(&now, 0);
    float sample_creation_time = timedifference_msec(start, now);

    /*READING THE ESTIMATION FROM EVERY DPU*/
    gettimeofday(&start, 0);

    uint64_t start_counting = 1;
    DPU_ASSERT(dpu_broadcast_to(dpu_set, "start_counting", 0, &start_counting, sizeof(start_counting), DPU_XFER_DEFAULT));

    //Launch the DPUs program one last time. The DPUs know that the file has ended and it is time to count the triangles
    DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));

    uint64_t single_dpu_triangle_estimation = 0;
    uint64_t total_triangle_estimation = 0;

    DPU_FOREACH(dpu_set, dpu) {
        DPU_ASSERT(dpu_copy_from(dpu, "triangle_estimation", 0, &single_dpu_triangle_estimation, sizeof(single_dpu_triangle_estimation)));
        total_triangle_estimation += single_dpu_triangle_estimation;
    }

    gettimeofday(&now, 0);

    float triangle_counting_time = timedifference_msec(start, now);

    //For debug purpose, get standard output from the DPUs
    DPU_FOREACH(dpu_set, dpu) {
        DPU_ASSERT(dpu_log_read(dpu, stdout));
    }

    if(TESTING){
        printf("%ld\n", total_triangle_estimation);
        printf("%f\n", setup_time);
        printf("%f\n", sample_creation_time);
        printf("%f\n", triangle_counting_time);
    }else{
        printf("Triangles: %ld\n", total_triangle_estimation);
        printf("Time for host setup(ms): %f\n", setup_time);
        printf("Time for creating the samples in the DPUs (ms): %f\n", sample_creation_time);
        printf("Time for counting the triangles in the DPUs (ms): %f\n", triangle_counting_time);
        printf("Total time (ms): %f\n", setup_time+sample_creation_time+triangle_counting_time);
    }

    // Close the file and free the memory taken by the BATCHES
    fclose(file_ptr);
    DPU_ASSERT(dpu_free(dpu_set));
}

void send_batch(struct dpu_set_t dpu_set, batch_t* batch){
    assert(batch != NULL);

    //Wait for all the DPUs to finish their task.
    //There is no problem if the program was never launched in the DPUs
    DPU_ASSERT(dpu_sync(dpu_set));

    //Send the batch to all dpus and run the dpu program on the current batch
    //The batch is at the start of the MRAM heap, and it will be overwritten during the sorting/locating phase
    DPU_ASSERT(dpu_broadcast_to(dpu_set, DPU_MRAM_HEAP_POINTER_NAME, 0, batch, sizeof(batch_t), DPU_XFER_DEFAULT));
    DPU_ASSERT(dpu_launch(dpu_set, DPU_ASYNCHRONOUS));
}

float timedifference_msec(struct timeval t0, struct timeval t1){
    return (t1.tv_sec - t0.tv_sec) * 1000.0f + (t1.tv_usec - t0.tv_usec) / 1000.0f;
}
