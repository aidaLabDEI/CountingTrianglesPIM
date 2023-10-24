#ifndef __COMMON_H__
#define __COMMON_H__

#include <stdint.h>

/*There may be two worst cases.
1: Each edge connects two new nodes
2: Each node is connected to the next, creating a "line"
In both cases, for each edge there is a new unique node, so,
considering 62MB free in the MRAM for the sample, considering that
for every edge 16 bytes are occupied, there can be 62MB/16B = 4063232 edges
*/
#ifndef MAX_SAMPLE_SIZE
#define MAX_SAMPLE_SIZE 4063232
#endif

//Struct used to transfer starting data from the host to the dpus. Aligned to 8 bytes
typedef struct {
    uint32_t random_seed;

    uint32_t sample_size;
    uint32_t n_colors;

    uint32_t hash_parameter_p;
    uint32_t hash_parameter_a;
    uint32_t hash_parameter_b;
} dpu_arguments_t;

//Contains the two nodes that make an edge
typedef struct {
    uint32_t u;
    uint32_t v;
} edge_t;

//Contains a pair of colors, representing the colors of an edge
typedef struct {
    uint32_t color_u;
    uint32_t color_v;
} edge_colors_t;

#endif /* __COMMON_H__ */
