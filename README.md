# CountingTrianglePIM

## Modyfing [Makefile](Makefile):
  - Change the number of tasklets per DPU
  - Change the number of available DPUs

## Execute the code:

  To execute the code, it is necessary to run, inside the folder (bin) created after make, the following command:
  ```
  ./app seed sample_size nr_colors path_to_graph_file
  ```
  where:

  - _seed_ is the seed for generating random numbers. Put 0 for a random seed
  - _sample\_size_ is the size of the sample inside the DPUs. Put 0 for the maximum allowed size
  - _nr\_colors_ is the number of colors used to color the
  - _path\_to\_graph\_file_ is the path to the file containing the edges of the graph.
  Each line of the file must contain an edge composed of two 32-bit integers and a space between the two numbers

## Other modifications:
- It is possible to modify the size of the buffer in the WRAM by modifying the value _WRAM\_BUFFER\_SIZE_ inside [dpu_util.h](dpu/dpu_util.h). 
If the product between the number of tasklets per DPU and the size of the buffer in the WRAM is too large, an error will occur. Try to lower one of the two values.
- It is possible to modify the size of the batch sent from the host to the DPUs by changing the value of _EDGES\_IN\_BATCH_ inside [common.h](common/common.h)
