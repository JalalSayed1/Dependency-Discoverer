# Dependency Discoverer

## About the project

- Processes the c/yacc/lex source file arguments, outputting the dependencies between the corresponding .o file, the .c source file, and any included .h files.
- Each .h file is also processed to yield a dependency between it and any included .h files.
- These dependencies are written to standard output in a form compatible with make.
- For example, assume that `foo.c` includes `inc1.h`, and `inc1.h` includes `inc2.h` and `inc3.h`; this results in

```
foo.o: foo.c inc1.h inc2.h inc3.h
```

## Background

- Some systems are extremely large, and it is difficult to keep the dependencies in the Makefile correct as many people make changes simultaneously. Therefore, there is a need for a program that can crawl over source files, noting any #include directives, recurse through files specified in #include directives, and finally generate the correct dependency specifications.
- Large-scale systems developed in C and C++ tend to include a large number of .h files, both of a system variety (enclosed in < >) and non-system (enclosed in “ ”).
- #include directives for system files (enclosed in < >) are NOT specified in dependencies.

## Specification

- For very large software systems, a singly-threaded application to crawl the source files may take a long time.
- **For the Sequential execution:**
  - The main() function may take the following arguments:

| Argument      | Description |
| -----------   | ----------- |
| Idir          | indicates a directory to be searched for any include files encountered.        |
| file.ext      | source file to be scanned for #include directives; ext must be c, y, or l.        |

The usage string is
```
$ ./dependencyDiscoverer [-Idir] file1.ext [file2.ext …]
```

- **The crawler**:
  -  uses the following environment variables when it runs:

| Env variable      | Description |
| -----------   | ----------- |
| CRAWLER_THREADS | if this is defined, it specifies the number of **worker threads** that the application must create. </br> if it is not defined, then two worker threads should be created. |
| CPATH | if this is defined, it contains a list of directories separated by ‘ : ’, these directories are to be searched for files specified in #include directives. </br> if it is not defined, then no additional directories are searched beyond the current directory and any specified by `–Idir` flags. |

> To set an environment variable in shell, use command: </br> `$ export CRAWLER_THREADS=3`

  - For example, if CPATH is `/home/user/include:/usr/local/group/include` and 
if `-Ikernel` is specified on the command line, then when processing
		`#include "x.h"`, x.h will be located by searching for it in the following order:
    - ./x.h
    - kernel/x.h
    - /home/user/include/x.h
    - /usr/local/group/include/x.h

## Design and Implementation

- Using a leader/worker concurrency pattern.
  - The main thread (leader) places file names to be processed in the work queue.
  - Worker threads select a file name from the work queue, scan the file to discover dependencies, add these dependencies to the result Hash Map and, if new, to the work queue.
- The key data structures, data flow, and threads in the concurrent version are shown in the figure below.

![image](https://user-images.githubusercontent.com/92950538/201767132-a717db1b-ceea-4e8f-a354-dc08e6a93342.png)


- It should be possible to adjust the number of worker threads that process the accumulated work queue to speed up the processing.
- Since the Work Queue and the Hash Map are shared between threads, a concurrency control mechanisms has been implemented to make a thread-safe access to them.

## How to run

```
$ cd <version_folder>
$ make dependencyDiscoverer
$ ./dependencyDiscoverer *.y *.l *.c
```

## To test it

```
$ cd test
$ ../<version_folder>/dependencyDiscoverer *.y *.l *.c | diff - output
```
Where output is the file containing the expected results.
