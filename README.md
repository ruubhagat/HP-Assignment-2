# HP-Assignment-2
Directory-Based Cache Coherence Protocol Simulation

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
