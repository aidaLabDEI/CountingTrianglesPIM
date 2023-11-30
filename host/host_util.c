#include <stdint.h>

#include "mg_hashtable.h"
#include "host_util.h"

void usage(){

    printf("Triangle Counting on the UPMEM architecture\n\n");
    printf("Usage:\n\n");
    printf(" -s # [Seed # is used for the random number generator. Random if not given]\n");
    printf(" -M # [The sample size inside the DPUs is #. Maximum allowed value if not given]\n");
    printf(" -p # [Edges are kept with probability #. No edges are ignored (p = 1) if not given]\n");

    printf(" -k # [The dictionary for Misra-Gries for each thread has a maximum of # entries. Misra-Gries is not used if not given]\n");
    printf(" -t # [Send a maximum of # top frequent nodes to the DPUs. Ignored if Misra-Gries is not used. Default value is 5]\n");

    printf(" -c # [Use # colors to color the nodes of the graph. Required]\n");
    pritnf(" -f <filename> [Input Graph in Matrix Market format. Required]\n");
    exit(1);
}

float timedifference_msec(struct timeval t0, struct timeval t1){
    return (t1.tv_sec - t0.tv_sec) * 1000.0f + (t1.tv_usec - t0.tv_usec) / 1000.0f;
}

uint64_t get_free_memory(){
    FILE *meminfo = fopen("/proc/meminfo", "r");
    if(meminfo == NULL){
        printf("Cannot read the amount of available memory.\n");
        exit(1);
    }

    char line[256];
    uint64_t ramKB = 0;
    while(fgets(line, sizeof(buff), meminfo)){
        if(sscanf(buff, "MemAvailable: %ld kB", &ramKB) == 1){
            break;
        }
    }

    if(fclose(meminfo) != 0){
        printf("Cannot close file containing the memory informations.\n");
        exit(1);
    }

    return ramKB * 1024;  //Return number of bytes
}

uint32_t global_top_freq(node_frequency_t** top_freq_th, node_frequency_t* result_top_f, uint32_t t){

    //Can hold all the top frequencies from the threads
    node_freq_hashtable_t top_freq = create_hashtable(NR_THREADS * 2 * t);

    int valid_nodes = 0;
    //For every top frequent node id in each thread
    for(int th_id = 0; th_id < NR_THREADS; th_id++){
        for(int i = 0; i < 2 * t; i++){

            //The cell is invalid
            if(top_freq_th[th_id][i].frequency <= 0){
                continue;
            }

            update_global_top_frequency(&top_freq, top_freq_th[th_id][i].node_id, top_freq_th[th_id][i].frequency);
            valid_nodes++;
        }
    }

    //Select the top t edges to return to the main thread
    //No need to return all top k if only a few are used
    for(int i = 0; i < t; i++){
        // Find the maximum element in unsorted array
        int max_idx = i;
        for(uint32_t j = i+1; j < top_freq.size; j++){
            if(top_freq.table[j].frequency > top_freq.table[max_idx].frequency){
              max_idx = j;
            }
        }

        all_top_freq[i] = top_freq.table[max_idx];

        if(max_idx != i){
            node_frequency_t temp = top_freq.table[max_idx];
            top_freq.table[max_idx] = top_freq.table[i];
            top_freq.table[i] = temp;
        }
    }

    delete_hashtable(&top_freq);

    return (valid_nodes > t ? t : valid_nodes);
}
