// This is my own work as defined in the Academic Ethics agreement I have signed.

/*
 * usage: ./dependencyDiscoverer [-Idir] ... file.c|file.l|file.y ...
 *
 * dependencyDiscoverer uses the CPATH environment variable, which can contain a
 * set of directories separated by ':' to find included files
 * if any additional directories are specified in the command line,
 * these are prepended to those in CPATH, left to right
 *
 * for example, if CPATH is "/home/user/include:/usr/local/group/include",
 * and if "-Ifoo/bar/include" is specified on the command line, then when
 * processing
 *           #include "x.h"
 * x.h will be located by searching for the following files in this order
 *
 *      ./x.h
 *      foo/bar/include/x.h
 *      /home/user/include/x.h
 *      /usr/local/group/include/x.h
 */

/*
 * general design of main()
 * ========================
 * There are three globally accessible variables:
 * - dirs: a vector storing the directories to search for headers
 * - theTable: a hash table mapping file names to a list of dependent file names
 * - workQ: a list of file names that have to be processed
 *
 * 1. look up CPATH in environment
 *
 * 2. assemble dirs vector from ".", any -Idir flags, and fields in CPATH (if it is defined)
 *
 * 3. for each file argument (after -Idir flags)
 *    a. insert mapping from file.o to file.ext (where ext is c, y, or l) into
 *       table
 *    b. insert mapping from file.ext to empty list into table
 *    e.g. file.o -> file.ext and then file.ext-> []
 *    c. append file.ext on workQ
 *
 * 4. for each file on the workQ
 *    a. lookup list of dependencies
 *    b. invoke process(name, list_of_dependencies)
 *
 * 5. for each file argument (after -Idir flags)
 *    a. create a hash table in which to track file names already printed
 *    b. create a linked list to track dependencies yet to print
 *    c. print "foo.o:", insert "foo.o" into hash table
 *       and append "foo.o" to linked list
 *    d. invoke printDependencies()
 *
 * general design for process()
 * ============================
 *
 * 1. open the file
 *
 * 2. for each line of the file
 *    a. skip leading whitespace
 *    b. if match "#include"
 *       i. skip leading whitespace
 *       ii. if next character is '"'
 *           * collect remaining characters of file name (up to '"')
 *           * append file name to dependency list for this open file
 *           * if file name not already in the master Table
 *             - insert mapping from file name to empty list in master table
 *             - append file name to workQ
 *
 * 3. close file
 *
 * general design for printDependencies()
 * ======================================
 *
 * 1. while there is still a file in the toProcess linked list
 *
 * 2. fetch next file from toProcess
 *
 * 3. lookup up the file in the master table, yielding the linked list of dependencies
 *
 * 4. iterate over dependencies
 *    a. if the filename is already in the printed hash table, continue
 *    b. print the filename
 *    c. insert into printed
 *    d. append to toProcess
 *
 * Additional helper functions
 * ===========================
 *
 * dirName() - appends trailing '/' if needed
 * parseFile() - breaks up filename into root and extension
 * openFile()  - attempts to open a filename using the search path defined by the dirs vector.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <mutex>
#include <thread>

// list of dirs:
std::vector<std::string> dirs;

// 2. create a "struct"  stores the "container" (data structures) and "sync" (lock) utilities:
struct ThreadSafeWorkQ {

private:
    std::list<std::string> workQ;
    // lock for the workQ:
    std::mutex workQ_mutex;

public:
    // 1. make thread safe workQ structure.
    void safe_push_back(std::string s) {
        workQ_mutex.lock();
        workQ.push_back(s);
        workQ_mutex.unlock();
    }

    int safe_size() {
        workQ_mutex.lock();
        int size = workQ.size();
        workQ_mutex.unlock();
        return size;
    }

    std::string safe_front() {
        workQ_mutex.lock();
        std::string front = workQ.front();
        workQ_mutex.unlock();
        return front;
    }

    void safe_pop_front() {
        workQ_mutex.lock();
        workQ.pop_front();
        workQ_mutex.unlock();
    }
};

struct ThreadSafeTheTable {

private:
    std::unordered_map<std::string, std::list<std::string>> theTable;
    // lock for the theTable:
    std::mutex theTable_mutex;

public:
    // 1. make thread safe theTable structure:
    auto safe_find(std::string s) {
        theTable_mutex.lock();
        auto it = theTable.find(s);
        theTable_mutex.unlock();
        return it;
    }

    void safe_insert(std::pair<std::string, std::list<std::string>> pair) {
        theTable_mutex.lock();
        theTable.insert(pair);
        theTable_mutex.unlock();
    }

    auto safe_end() {
        theTable_mutex.lock();
        auto it = theTable.end();
        theTable_mutex.unlock();
        return it;
    }
};

// 3. provide similar interface to the container but with appropriate "synchronisation":
ThreadSafeWorkQ workQ;
ThreadSafeTheTable theTable;

/**
 * @brief appends trailing '/' if needed
 *
 * @param c_str directory string
 * @return std::string
 */
std::string dirName(const char *c_str) {
    std::string s = c_str; // s takes ownership of the string content by allocating memory for it
    if (s.back() != '/') {
        s += '/';
    }
    return s;
}

/**
 * @brief breaks up filename into root and extensions
 *
 * @param c_file filename string
 * @return std::pair<std::string, std::string>
 */
std::pair<std::string, std::string> parseFile(const char *c_file) {
    std::string file = c_file;
    std::string::size_type pos = file.rfind('.');
    // std::string::npos means end of strings:
    if (pos == std::string::npos) {
        return {file, ""};
    } else {
        return {file.substr(0, pos), file.substr(pos + 1)};
    }
}

/**
 * @brief open file using the directory search path constructed in main()
 *
 * @param file filename string
 * @return FILE* file pointer
 */
static FILE *openFile(const char *file) {
    FILE *fd;
    // loop over the dirs vector:
    for (unsigned int i = 0; i < dirs.size(); i++) {
        // construct the full path:
        std::string path = dirs[i] + file;
        // try to open the file:
        fd = fopen(path.c_str(), "r");
        if (fd != NULL)
            return fd; // return the first file that successfully opens
    }
    return NULL;
}

/**
 * @brief process file, looking for #include "foo.h" lines
 *
 * @param file filename string
 * @param ll list of dependencies
 */
static void process(const char *file, std::list<std::string> *ll) {
    // buf to hold each line of the file and name to hold the filename (if match is found):
    char buf[4096], name[4096];
    // 1. open the file
    FILE *fd = openFile(file);
    if (fd == NULL) {
        fprintf(stderr, "Error opening %s\n", file);
        exit(-1);
    }
    // 2. for each line of the file, read it into buf:
    while (fgets(buf, sizeof(buf), fd) != NULL) {
        char *p = buf;
        // 2a. skip leading whitespace
        while (isspace((int)*p)) {
            p++;
        }
        // 2b. if match #include
        // compare 8 characters starting from p with "#include":
        if (strncmp(p, "#include", 8) != 0) {
            // if not match, continue the while loop to next line:
            continue;
        }
        p += 8; // point to first character past #include
        // 2bi. skip leading whitespace
        while (isspace((int)*p)) {
            p++;
        }

        // if next character is not '"', continue the while loop to next line:
        if (*p != '"') {
            continue;
        }
        // 2bii. next character is a "
        p++; // skip "
        // 2bii. collect remaining characters of file name
        char *q = name;
        while (*p != '\0') {
            if (*p == '"') {
                break;
            }
            // copy filename from p to q:
            *q++ = *p++;
        }
        *q = '\0';
        // 2bii. append file name to dependency list
        ll->push_back({name});
        // 2bii. if file name not already in table ...
        if (theTable.safe_find(name) != theTable.safe_end()) {
            // if it already exists, continue the while loop to next line:
            continue;
        }
        // ... insert mapping from file name to empty list in table ...
        theTable.safe_insert({name, {}});
        // ... append file name to workQ
        workQ.safe_push_back(name);
    }
    // 3. close file
    fclose(fd);
}

/**
 * @brief iteratively print dependencies
 *
 * @param printed a set of unique objects of type std::string
 * @param toProcess a list of filenames to process
 * @param fd file pointer
 */
static void printDependencies(std::unordered_set<std::string> *printed,
                              std::list<std::string> *toProcess,
                              FILE *fd) {
    // if any of the pointers are null, return:
    if (!printed || !toProcess || !fd)
        return;

    // 1. while there is still a file in the toProcess list
    while (toProcess->size() > 0) {
        // 2. fetch next file to process
        std::string name = toProcess->front();
        toProcess->pop_front();
        // 3. lookup file in the table, yielding list of dependencies.
        // presuming the file name is in the table as it was inserted in process():
        // get the list only:
        std::list<std::string> *ll = &(theTable.safe_find(name)->second);
        // 4. iterate over dependencies of this name (filename):
        for (auto iter = ll->begin(); iter != ll->end(); iter++) {
            // 4a. if filename is already in the printed table, continue:
            if (printed->find(*iter) != printed->end()) {
                continue;
            }
            // 4b. print filename
            // c_str() returns a pointer to an array that contains a null-terminated sequence of characters representing the current value of the basic_string object.
            fprintf(fd, " %s", iter->c_str());
            // 4c. insert into printed
            printed->insert(*iter);
            // 4d. append to toProcess
            // append each dependency of the current filename to the toProcess list:
            toProcess->push_back(*iter);
        }
    }
}

int main(int argc, char *argv[]) {
    // 1. look up CPATH in environment
    // set CPATH on command line, if needed, with e.g. 'export CPATH=/usr/include:/usr/local/include'
    char *cpath = getenv("CPATH");

    // get CRAWLER_THREADS from environment, if set:
    char *tempNumOfThreads = getenv("CRAWLER_THREADS");
    int numOfThreads = 0;
    if (tempNumOfThreads) {
        numOfThreads = atoi(tempNumOfThreads);
    }

    // determine the number of -Idir arguments
    int i;
    for (i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-I", 2) != 0)
            break;
    }
    int start = i;

    // 2. start assembling dirs vector
    dirs.push_back(dirName("./")); // always search current directory first
    for (i = 1; i < start; i++) {
        dirs.push_back(dirName(argv[i] + 2 /* skip -I */));
    }
    if (cpath != NULL) {
        // str(cpath) means convert cpath to a string:
        std::string str(cpath);
        // ::size_type guarantees the string to be large enough to represent the sizes of any strings:
        std::string::size_type last = 0;
        std::string::size_type next = 0;
        // loop over the cpath string, looking for ':' as a delimiter:
        while ((next = str.find(":", last)) != std::string::npos) {
            dirs.push_back(str.substr(last, next - last));
            last = next + 1;
        }
        dirs.push_back(str.substr(last));
    }
    // 2. finished assembling dirs vector

    // 3. for each file argument ...
    for (i = start; i < argc; i++) {
        std::pair<std::string, std::string> pair = parseFile(argv[i]);
        if (pair.second != "c" && pair.second != "y" && pair.second != "l") {
            fprintf(stderr, "Illegal extension: %s - must be .c, .y or .l\n",
                    pair.second.c_str());
            return -1;
        }

        std::string obj = pair.first + ".o";

        // 3a. insert mapping from file.o to file.ext
        theTable.safe_insert({obj, {argv[i]}});

        // 3b. insert mapping from file.ext to empty list
        theTable.safe_insert({argv[i], {}});

        // 3c. append file.ext on workQ
        workQ.safe_push_back(argv[i]);
    }

    if (numOfThreads > 0) {

        // vector of threads:
        std::vector<std::thread> threads;

        // create a thread:

        for (int i = 0; i < numOfThreads; i++) {

            auto thread = std::thread([start, argc, argv]() -> int {
                // 4. for each file on the workQ
                while (workQ.safe_size() > 0) {
                    std::string filename = workQ.safe_front();
                    workQ.safe_pop_front();

                    if (theTable.safe_find(filename) == theTable.safe_end()) {
                        fprintf(stderr, "Mismatch between table and workQ\n");
                        return -1;
                    }

                    // 4a&b. lookup dependencies and invoke 'process'
                    // get the list only:
                    process(filename.c_str(), &(theTable.safe_find(filename)->second));
                }

                int i;
                // 5. for each file argument
                for (i = start; i < argc; i++) {
                    // 5a. create hash table in which to track file names already printed
                    std::unordered_set<std::string> printed;
                    // 5b. create list to track dependencies yet to print
                    std::list<std::string> toProcess;

                    std::pair<std::string, std::string> pair = parseFile(argv[i]);

                    std::string obj = pair.first + ".o";
                    // 5c. print "foo.o:" ...
                    printf("%s:", obj.c_str());
                    // 5c. ... insert "foo.o" into hash table and append to list
                    printed.insert(obj);
                    toProcess.push_back(obj);
                    // 5d. invoke
                    printDependencies(&printed, &toProcess, stdout);

                    printf("\n");
                }
                return 0;
            });

            threads.push_back(std::move(thread));
        }
        /* end of thread 1 */

        // wait for the thread to finish:
        for (auto &thread : threads) {
            thread.join();
        }

        // if no threads are specified, run the code sequentially:
    } else {
        // 4. for each file on the workQ
        while (workQ.safe_size() > 0) {
            std::string filename = workQ.safe_front();
            workQ.safe_pop_front();

            if (theTable.safe_find(filename) == theTable.safe_end()) {
                fprintf(stderr, "Mismatch between table and workQ\n");
                return -1;
            }

            // 4a&b. lookup dependencies and invoke 'process'
            // get the list only:
            process(filename.c_str(), &(theTable.safe_find(filename)->second));
        }

        int i;
        // 5. for each file argument
        for (i = start; i < argc; i++) {
            // 5a. create hash table in which to track file names already printed
            std::unordered_set<std::string> printed;
            // 5b. create list to track dependencies yet to print
            std::list<std::string> toProcess;

            std::pair<std::string, std::string> pair = parseFile(argv[i]);

            std::string obj = pair.first + ".o";
            // 5c. print "foo.o:" ...
            printf("%s:", obj.c_str());
            // 5c. ... insert "foo.o" into hash table and append to list
            printed.insert(obj);
            toProcess.push_back(obj);
            // 5d. invoke
            printDependencies(&printed, &toProcess, stdout);

            printf("\n");
        }
    }

    return 0;
}
