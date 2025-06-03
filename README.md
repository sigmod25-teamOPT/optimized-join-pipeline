# Optimized Join Pipeline - SIGMOD Programming Contest 2025

Implementation of a cache-efficient, multithreaded, lock-free, hash-based join pipeline utilizing a memory-efficient hash table optimized for joins. This project was created for the SIGMOD 2025 Programming Competition, where our undergraduate team achieved 4th place globally! Our implementation achieves a 630x speedup over the organizers' reference solution.

## Contents
- [Optimized Join Pipeline - SIGMOD Programming Contest 2025](#optimized-join-pipeline---sigmod-programming-contest-2025)
  - [Contents](#contents)
  - [Our Team](#our-team)
  - [Preparation](#preparation)
  - [Building](#building)
  - [Execution](#execution)
  - [Task Overview](#task-overview)
  - [Implementation Details](#implementation-details)
    - [Loading and Preprocessing](#loading-and-preprocessing)
    - [Join Data Flow \& Materialization](#join-data-flow--materialization)
    - [Unchained Hash Table](#unchained-hash-table)
    - [Parallel Data Processing](#parallel-data-processing)
    - [Pipeline](#pipeline)
  - [References](#references)

## Our Team

Our undergraduate team, representing University of Athens, consists of the following members:

- [Yannis Xiros](https://github.com/yannisxiros)
- [Zisis Vakras](https://github.com/zisisvakras)
- [Alexandros Kostas](https://github.com/AlexKostas)

## Preparation

> [!TIP]
> Run all the following commands for this and the subsequent sections in the root directory of this project.

First, download the imdb dataset.

```bash
./download_imdb.sh
```

Second, prepare the DuckDB database for correctness checking.

```bash
./build/build_database imdb.db
```

## Building

Build the project with the following commands:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -Wno-dev
cmake --build build -- -j $(nproc)
```

## Execution

You can run some quick unit tests that verify the correct execution of the program with:

```bash
./build/unit_tests
```

Execute the full query set with:

```bash
./build/run plans.json
```

Run individual queries (e.g. queries `1a` and `1b`) using:

```bash
./build/run plans.json 1a 1b
```

## Task Overview

- Develop an efficient, in-memory join pipeline executor delivering high-performance across a wide range of hardware architectures and configurations.
- Evaluated on completely anonymized data and queries sampled from the Join Order Benchmark  (J.O.B. on the IMDB Dataset).
- Input Data provided as in-memory, column-store, paginated tables. Produced output follows the same format.
- Join Execution Plan provided and optimized by PostgreSQL as a directed tree of Scan Nodes - representing input tables - and Join Nodes.
- Joins are performed only on integer fields, while Doubles and Strings can also appear in the result. Attributes can have null values.

## Implementation Details

### Loading and Preprocessing

- Analyze the Plan to classify columns as join fields, output-only attributes or unused.
- Unused columns are discarded. Remaining columns require fast random access.
- The vast majority of columns are fully packed without null values and can be accessed in place in constant time with a single division.
- Iterators are used when such columns are accessed serially to eliminate the division on the hot path.
- Remaining columns are copied in a contiguous array to enable random access. 
- Strings are copied lazily only when they actually appear in the result using fast hardware instructions.
- Join execution can start early with output-only columns **loaded lazily** in the background.
- 99+% of columns never have to be loaded or are processed in the background, thereby approximating a completely **zero-copy** solution!
- Do not spend any time collecting stats in an attempt to beat Postgres’ plan optimizer.

### Join Data Flow & Materialization

- Late Record Materialization to eliminate unnecessary data copies to reduce cache pollution.
- Each join materializes the column needed for the next join in the pipeline to improve memory access patterns.
- Joins at the root of the tree materilaze the final output.

### Unchained Hash Table

- Hash table optimized for join [1].
- Since inserts and lookups are not mixed, separate building and probing into distinct phases.
- Partition Data among workers to parallelize building.
- All the entries inserted to the table live in one large contiguous block, massively improving cache locality.
- Use of CRC hardware instruction instead of off-the-shelf hash functions to hash in only a few cycles while maintaining adequate collision resistance.
- Built-in bloom filter in the lower 16-bits of directory pointer and use of precalculated tags for containment check.

### Parallel Data Processing

- Implement a Priority-Based Task Queue that dynamically assigns tasks on available hardware threads.
- Prioritize tasks that enable processing of interdependent joins.
- Morsel-driven Parallelism [2]: break down heavier jobs into self-contained tasks, each responsible for independently processing a smaller chunk of the data.
- Implement a lock-free chunk feeder mechanism to efficiently handle skew: tasks that finish early can dynamically “steal” pages from other tasks to balance workload.
- Each task independently produces paginated results and deposits them in dedicated per-worker slots without incurring any synchronization cost.
- Dynamic Job Batching for very small tasks to reduce overhead.
- Eliminate false sharing by properly aligning concurrently accessed data structures.

### Pipeline

- Intermediate results are chunked and immediately forwarded so the next join in the pipeline can also progress while the data are still hot in CPU’s Caches.
- Completely lock-free pipeline implemented using atomic instructions to minimize synchronization overhead and enable finer granularity tasks.
- Every completed task executes part of the pipeline control logic - appropriately advancing pipeline state and kickstarting tasks for next stages or joins.
- Schedule further processing of the data to the same hardware thread that produced it - optimizing cache efficiency.

## References

[1]: [Simple, Efficient, and Robust Hash Tables for Join Processing](https://db.in.tum.de/~birler/papers/hashtable.pdf)
[2]: [Morsel-Driven Parallelism: A NUMA-Aware Query Evaluation Framework for the Many-Core Age](https://db.in.tum.de/~leis/papers/morsels.pdf)
