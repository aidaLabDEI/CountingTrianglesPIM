#include <assert.h>  //Assert
#include <dpu.h>  //Create dpu set
#include <dpu_log.h>  //Get logs from dpus (used for debug)
#include <stdio.h>  //Print

//Handle file
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/mman.h>

#include <time.h>  //Random seed
#include <sys/time.h>  //Measure execution time
#include <stdlib.h>  //Various
#include <stdint.h>  //Known size integers
#include <stdbool.h>  //Booleans
#include <math.h>  //floor and ceil
#include <pthread.h>  //Threads

#include "../common/common.h"
#include "host_util.h"

#ifndef DPU_BINARY
#define DPU_BINARY "./task"
#endif

//If TESTING = 1, the output numbers have no description. Used to make easier to parse output when testing
#define TESTING 0

int main(int argc, char* argv[]){
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

    uint32_t color_number = atoi(argv[3]);  //Number of colors used to color the nodes

    //Number of triplets created given the colors. binom(c+2, 3)
    uint32_t triplets_created = round((1.0/6) * color_number * (color_number+1) * (color_number+2));
    if(triplets_created > NR_DPUS){
        printf("More triplets than DPUs. Use more DPUs or less colors. Given %d colors, no less than %d DPUs can be used.\n", color_number, triplets_created);
        return 1;
    }

    uint32_t sample_size_dpus = atoi(argv[2]);  //Size of the sample (number of edges) inside the DPUs
    if(sample_size_dpus > MAX_SAMPLE_SIZE){
        printf("Sample size too big. The limit is: %d.\n", MAX_SAMPLE_SIZE);
        return 1;
    }

    if(sample_size_dpus == 0){
        sample_size_dpus = MAX_SAMPLE_SIZE;
    }

    char* file_name = argv[4];

    /*READ THE FILE CONTAINING THE GRAPH*/

    //Start measuring time after handling the input
    struct timeval start;
    gettimeofday(&start, 0);

    FILE * file_ptr = fopen(file_name, "r");
    if(file_ptr == NULL){
        printf("File not found.\n");
        return 1;
    }

    char char_buffer[32];
    uint32_t edges_in_graph;

    //Read the first line that contains the number of edges in the graph
    if(NULL == fgets(char_buffer, sizeof(char_buffer), file_ptr)){
        printf("File is empty.\n");
        return 1;
    }
    sscanf(char_buffer, "%d", &edges_in_graph);

    edge_t* graph = (edge_t*) malloc(edges_in_graph * sizeof(edge_t));

    edge_t current_edge;
    uint32_t offset_in_graph = 0;
    uint32_t node1, node2;

    while (fgets(char_buffer, sizeof(char_buffer), file_ptr) != NULL) {  //Reads until EOF

        //Each edge is formed by two unsigned integers separated by a space
        sscanf(char_buffer, "%d %d", &node1, &node2);

        if(node1 < node2){  //Nodes in edge need to be ordered
            current_edge = (edge_t){node1, node2};
        }else{
            current_edge = (edge_t){node2, node1};
        }

        graph[offset_in_graph++] = current_edge;
    }

    struct timeval now;
    gettimeofday(&now, 0);
    float read_graph_time = timedifference_msec(start, now);
    printf("Time to read the graph file: %f\n", read_graph_time);
    gettimeofday(&start, 0);

    /*DPU INFO CREATION*/

    //Each thread has its own data, so no mutexes are needed while inserting
    dpu_info_t* dpu_info_array = malloc(sizeof(dpu_info_t) * NR_THREADS * NR_DPUS);

    //Each DPU will receive at maximum around (6/C^2) * edges_in_graph edges.
    //This number needs to be divided by the number of threads.
    //Multiplied by 1.5 to avoid errors due to unlucky distribution (especially in small graphs with many DPUs)
    //If the amount of edges per DPU per thread is too small, the multiplication is not enough to guarantee correctness
    uint32_t max_expected_edges_per_dpu_per_thread = 1.5 * ((6.0 / (color_number*color_number)) * edges_in_graph) / NR_THREADS + 2500;

    for(int th_id = 0; th_id < NR_THREADS; th_id++){
        for(int dpu_id = 0; dpu_id < NR_DPUS; dpu_id++){
            dpu_info_array[th_id*NR_DPUS + dpu_id].edge_count_batch = 0;
            dpu_info_array[th_id*NR_DPUS + dpu_id].batch = (edge_t*) malloc(max_expected_edges_per_dpu_per_thread * sizeof(edge_t));
        }
    }

    //Determine the parameters used to color the nodes
    uint32_t hash_parameter_p = 8191;  //Prime number
    uint32_t hash_parameter_a = rand() % (hash_parameter_p-1) + 1; // a in [1, p-1]
    uint32_t hash_parameter_b = rand() % hash_parameter_p; // b in [0, p-1]

    /*INITIALIZING DPUs*/
    struct dpu_set_t dpu_set, dpu;

    DPU_ASSERT(dpu_alloc(NR_DPUS, NULL, &dpu_set));
    DPU_ASSERT(dpu_load(dpu_set, DPU_BINARY, NULL));

    /*SENDING THE SAME STARTING ARGUMENTS TO ALL DPUs*/
    dpu_arguments_t input_arguments = {random_seed, sample_size_dpus, color_number, hash_parameter_p, hash_parameter_a, hash_parameter_b};
    DPU_ASSERT(dpu_broadcast_to(dpu_set, "DPU_INPUT_ARGUMENTS", 0, &input_arguments, sizeof(dpu_arguments_t), DPU_XFER_DEFAULT));

    /*SENDING IDS TO THE DPUS*/

    //Necessary to create an array of ids because each dpu xfer is associated with
    //an address and to send different values, each address should be different from the others.
    uint64_t ids[NR_DPUS];  //8bytes for transfer to the DPUs
    for(uint64_t i = 0; i < NR_DPUS; i++){
        ids[i] = i;
    }

    uint32_t index;
    DPU_FOREACH(dpu_set, dpu, index) {  //dpu is a set itself
        DPU_ASSERT(dpu_prepare_xfer(dpu, &ids[index]));  //Associate an address to each dpu
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_TO_DPU, "id", 0, sizeof(ids[0]), DPU_XFER_DEFAULT));

    //Launch DPUs for setup
    DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));

    gettimeofday(&now, 0);
    float setup_time = timedifference_msec(start, now);
    printf("Time for the setup: %f\n", setup_time);
    gettimeofday(&start, 0);

    /*SAMPLE CREATION*/

    pthread_mutex_t send_to_dpus_mutex;  //Prevent from copying data to the DPUs before the others have finished handling the previous batch
    if (pthread_mutex_init(&send_to_dpus_mutex, NULL) != 0) {
        printf("Mutex not initialised. Exiting.\n");
        return 1;
    }

    //Handle edges in different threads
    pthread_t threads[NR_THREADS];
    handle_edges_thread_args_t he_th_args[NR_THREADS];  //Arguments for the threads

    uint32_t edges_per_thread = edges_in_graph/NR_THREADS;  //Number of edges that each thread will handle

    for(int th_id = 0; th_id < NR_THREADS; th_id++){

        ///Determine the sections of edges that each thread will consider
        uint32_t from_edge = edges_per_thread * th_id;
        uint32_t to_edge;  //Not included
        if(th_id != NR_THREADS-1){
            to_edge = edges_per_thread * (th_id+1);
        }else{
            to_edge = edges_in_graph;  //If last thread, handle all remaining edges
        }

        he_th_args[th_id] = (handle_edges_thread_args_t){th_id, graph, from_edge, to_edge, &input_arguments, dpu_info_array, &dpu_set, &send_to_dpus_mutex};

        pthread_create(&threads[th_id], NULL, handle_edges_file, (void *)&he_th_args[th_id]);
    }

    //Wait for all threads to finish
    for(uint32_t i = 0; i < NR_THREADS; i++){
        pthread_join(threads[i], NULL);
    }

    //The threads launched the last batches, need to wait for them to be processed
    DPU_ASSERT(dpu_sync(dpu_set));

    gettimeofday(&now, 0);
    float sample_creation_time = timedifference_msec(start, now);
    printf("Time for the sample creation: %f\n", sample_creation_time);

    /*READING THE ESTIMATION FROM EVERY DPU*/
    gettimeofday(&start, 0);

    //Signal the DPUs to start counting
    uint64_t start_counting = 1;
    DPU_ASSERT(dpu_broadcast_to(dpu_set, "start_counting", 0, &start_counting, sizeof(start_counting), DPU_XFER_DEFAULT));

    //Launch the DPUs program one last time. The DPUs know that the file has ended and it is time to count the triangles
    DPU_ASSERT(dpu_launch(dpu_set, DPU_SYNCHRONOUS));

    uint64_t single_dpu_triangle_estimation[NR_DPUS];

    DPU_FOREACH(dpu_set, dpu, index) {
        DPU_ASSERT(dpu_prepare_xfer(dpu, &single_dpu_triangle_estimation[index]));
    }
    DPU_ASSERT(dpu_push_xfer(dpu_set, DPU_XFER_FROM_DPU, "triangle_estimation", 0, sizeof(single_dpu_triangle_estimation[0]), DPU_XFER_DEFAULT));

    uint64_t total_triangle_estimation = 0;

    //First color in the triplet section considered
    int first_triplet_color = 0;
    //id of the triplet (and DPU) that counts the triangle whose nodes are colored with the same color
    uint32_t same_color_triplet_id = 0;

    for(uint32_t dpu_id = 0; dpu_id < NR_DPUS; dpu_id++){

        int32_t addition_multiplier = 1;

        if(dpu_id < triplets_created){

            if(dpu_id == same_color_triplet_id){
                addition_multiplier = 2 - color_number;

                //Add binom(C + 1 - first_triplet_color, 2) to the previous id
                same_color_triplet_id += 0.5 * (color_number - first_triplet_color) * (color_number - first_triplet_color + 1);
                first_triplet_color++;
            }
        }

        total_triangle_estimation += single_dpu_triangle_estimation[dpu_id] * addition_multiplier;
    }

    gettimeofday(&now, 0);

    float triangle_counting_time = timedifference_msec(start, now);
    printf("Time to count the triangles: %f\n", triangle_counting_time);

    printf("Triangles: %ld", total_triangle_estimation);

    //For debug purpose, get standard output from the DPUs
    /*DPU_FOREACH(dpu_set, dpu) {
        DPU_ASSERT(dpu_log_read(dpu, stdout));
    }*/

    if(TESTING){
        printf("%ld\n", total_triangle_estimation);
        printf("%f\n", setup_time);
        printf("%f\n", sample_creation_time);
        printf("%f\n", triangle_counting_time);
        printf("%ld\n", total_triangle_estimation);
    }

    //Free everything
    DPU_ASSERT(dpu_free(dpu_set));

    for(int th_id = 0; th_id < NR_THREADS; th_id++){
        for(int dpu_id = 0; dpu_id < NR_DPUS; dpu_id++){
            free(dpu_info_array[th_id * NR_DPUS + dpu_id].batch);
        }
    }

    free(dpu_info_array);

    pthread_mutex_destroy(&send_to_dpus_mutex);
}
