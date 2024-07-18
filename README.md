# CountingTrianglePIM

The code was tested using using the [UPMEM SDK version 2024.1.0](https://sdk.upmem.com/) both on real hardware and on the provided functional model.

## Modyfing [Makefile](Makefile):
  - Change the number of tasklets per DPU by modifying `NR_TASKLETS`. The value should be a power of 2. The best performing configuration uses 16 tasklets.
  - Change the number of available DPUs by modifying `NR_DPUS`. The number of DPUs to use should be a valid value, calculated given the number $C$ of colors used, using the formula `NR_DPUS = Binom(C+2, 3)`.
  - Change the number of threads used by the host processor by modifying `NR_THREADS`. The best configuration uses a number of threads equal to the number of threads available in the host CPU.

## Execute the code:

  To execute the code, it is necessary to run, inside the folder (bin) created after running `make`, the following command:
  ```
  ./app -s seed -M sample_size -p keep_percentage -k Misra_Gries_dictionary_size -t nr_most_frequent_nodes_sent -c nr_colors -f nr_updates path_to_graph_files
  ```
  where:

  - _seed_ is the seed for generating random numbers. Random if not given.
  - _sample\_size_ is the size of the sample inside the DPUs. The maximum allowed value is used if not given.
  - _keep\_percentage_ is the probability of an edge being kept. No edges are ignored (_keep\_percentage_ = 1) if not given.
  - _Misra\_Gries\_dictionary\_size_ is the number of maximum entries in the dictionary for Misra-Gries for each thread. Misra-Gries is not used if not given.
  - _nr\_most\_frequent\_nodes\_sent_ is the number of top frequent nodes sent to the DPUs. It is ignored if Misra-Gries is not used. The default value is 5.
  - _nr\_colors_ is the number of colors used to color the graph. It is also used to determine how many DPUs will be used. **Required**.
  - _nr\_updates_ is the number of files that contain updates to the graph. **Required**.
  - _path\_to\_graph\_files_ are the paths to the files containing the edges of the updates to the graph in COO format. **Required**.

## Other modifications:
- It is possible to modify the size of the buffer in the WRAM by modifying the value _WRAM\_BUFFER\_SIZE_ inside [dpu_util.h](dpu/dpu_util.h). Do not exceed 2048 bytes.
