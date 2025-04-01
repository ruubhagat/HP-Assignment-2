<<<<<<< HEAD
# HP-Assignment-2
Directory-Based Cache Coherence Protocol Simulation
=======
# Directory-Based Cache Coherence Protocol Simulation

In this assignment, you will be implementing a directory-based cache coherence protocol in a distributed shared memory ( DSM ) system. The protocol which you will be implementing is based off of [DASH](https://dl.acm.org/doi/10.1145/325096.325132), but is not 1:1 replica. The implementation details will be explained in the subsequent sections. The system consists of multiple processor nodes, which will be simulated using OpenMP. Each processor contains:
- Local cache   : direct-mapped, with `CACHE_SIZE` entries
- Local memory
- Directory     : maintains coherence state of memory blocks
  
Each processor also contains a message queue, which will be shared in this simulation, as you will not be building a separate interconnect model for message passing.Instead, the processors will be communicating through these message queues/buffers. In order to ensure cache coherence between these nodes, you will be implementing a cache coherence protocol.

## Coherence State

Each directory entry is associated with a memory line/block, and also has a state. The system follows MESI protocol for the cachelines, 
- MODIFIED  :   cached in one processor, dirty
- EXCLUSIVE :   cached in one processor, clean
- SHARED    :   cached in multiple processors, clean
- INVALID   :   not present in cache / not a valid value

and uses the following states for the directory entry:
- EM    : EXCLUSIVE or MODIFIED : present in one cache, can be clean or dirty
- S     : SHARED    : present in multiple caches, clean
- U     : UNOWNED   : not cached in any processor

## Directory Entry

Each directory entry contains:
- bitmap    : indicates which processor nodes contain the associated memory block in their caches
- state     : EM, S or U

## Transaction Types

| Transaction Type     | Description |
|----------------------|-------------|
| `READ_REQUEST`      | Requesting node sends to home node on a read miss. |
| `WRITE_REQUEST`     | Requesting node sends to home node on a write miss. |
| `REPLY_RD`         | Home node replies with data to requestor for read request. |
| `REPLY_WR`         | Home node replies to requestor for write request. |
| `REPLY_ID`         | Home node replies with IDs of sharers to requestor. |
| `INV`              | Owner node asks sharers to invalidate. |
| `UPGRADE`          | Owner node asks home node to change state to EM. |
| `WRITEBACK_INV`    | Home node asks owner node to flush and change to INVALID. |
| `WRITEBACK_INT`    | Home node asks owner node to flush and change to SHARED. |
| `FLUSH`            | Owner flushes data to home + requestor. |
| `FLUSH_INVACK`     | Flush, piggybacking an InvAck message. |
| `EVICT_SHARED`     | Handle cache replacement of a shared cache line. |
| `EVICT_MODIFIED`   | Handle cache replacement of a modified cache line. |

## Instructions and Input Format

You will be working with a custom memory address space, which will be of size 1 byte. MSB 4 bits indicate the processor on which the block is present, and LSB 4 bits indicate the memory index. For example, `0x17` refers to the 7th memory block in processor 1. This addressing scheme allows for a maximum of 16 processors, each with 16 memory locations, operating concurrently in the system. Since `bitVector` is of size 1 byte, we further limit the maximum number of processors to 8. Address `0xFF` is considered invalid, meaning there is no memory block at that cacheline.

**Since we are using type `byte` ( `char` ), for the `value` field, we can read/write values in the range `0-255`.**

The simulator will be fed input files with a name of the format `core_n.txt`, where `n` represents the core/node number. So `core_0.txt` will represent the input to core 0, for example. All core files will be in `tests/<test_directory>`. `n + 1` will be the total number of threads we support. Each of the files will be read by individual threads which will run the instructions.

You will be dealing with two types of instructions: `RD <address>` and `WR <address> <value>`

Consider the following example,

`core_0.txt`:

```
WR 0x15 100
RD 0x17
```

The first instruction indicates that core 0 is trying to write the value 100 to memory location `0x15`. The second instruction indicates that core 0 is trying to read the value stored at memory location `0x17`. 

Note that, while the memory is distributed across nodes, the memory regions combined act as a global shared memory space. Meaning, core 0 can read content from the memory region in node 2, for example.

## Output

You will have to use `printProcessorState` function to dump the contents of the processor, after it has processed all the instructions. **IMPORTANT: DO NOT MODIFY THE FUNCTION, EVALUATION WILL BE BASED OFF OF THIS OUTPUT.** Note that you can use `printProcState` to dump multiple times, ensuring that instructions and message handling of all processors in complete. Evaluation will be done in a transparent manner, and any loss of marks will be explained.

**IMPORTANT: Please wrap all debug statements within `#ifdef DEBUG` and `#endif`.** Example:
```
#ifdef DEBUG
printf( "Processor %d msg from: %d, type: %d, address: 0x%02X\n",
        threadId, msg.sender, msg.type, msg.address );
#endif /* ifdef DEBUG */
```

The final output should only contain the dump from `printProcessorState`.
>>>>>>> e9482c6 (Initial commit)

## How to Run

Regular build:
```
gcc -fopenmp -o cache_simulator assignment.c
```

Debug build:
```
gcc -fopenmp -o cache_simulator assignment.c -O0 -D DEBUG_MSG -D DEBUG_INSTR -g
```

Execution:
```
./cache_simulator <test_directory>
```

Example:
```
./cache_simulator sample
```
