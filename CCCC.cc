#include <stdint.h>
#include <math.h>
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <sstream>
#include "PerfUtils/Cycles.h"
#include "PerfUtils/Util.h"

/**
 * This program attempts to measure unidirectional cross core communication
 * cost (CCCC) for all pairs of cores.
 *
 * The following algorithm is used here.
 *    0. Sender pins itself to a core.
 *    2. Sender waits for the receiver to indicate readiness.
 *    1. Receiver starts in a different thread.
 *    3. Receiver pins itself to a core
 *    4. Receiver sets a flag to let the sender know that it is ready.
 *    5. Receiver starts polling on the shared memory location.
 *    6. The sender writes the Cycle counter to the shared memory location, and
 *       then waits for the receiver to signal that it has finished.
 *    7. As soon as receiver reads the timestamp, it will read the timestamp
 *       and compute and store a difference. It will then clear the flag to
 *       signal to the sender that it is done.
 */

#define NUM_WARMUP 10000
#define NUM_RUNS 3000000

using PerfUtils::Cycles;

void recv(uint64_t coreId, std::atomic<uint64_t>* timestamp, uint64_t** output){
    PerfUtils::Util::pinThreadToCore(coreId);
    uint64_t* cycleDifferences = (uint64_t*)malloc(NUM_RUNS*sizeof(uint64_t));
    uint64_t endTime;

    for (int i = 0; i < NUM_WARMUP; i++) {
        *timestamp = 0;
        while (*timestamp == 0);
    }
    for (int i = 0; i < NUM_RUNS; i++) {
        *timestamp = 0;
        while (*timestamp == 0);
        endTime = Cycles::rdtsc();
        cycleDifferences[i] = endTime - *timestamp;
    }
    *output = cycleDifferences;
}

void send(uint64_t coreId, std::atomic<uint64_t>* timestamp) {
    PerfUtils::Util::pinThreadToCore(coreId);
    for (int i = 0; i < NUM_RUNS + NUM_WARMUP; i++) {
        while (*timestamp != 0);
        *timestamp = Cycles::rdtsc();
    }
}

int compare(const void * a, const void * b)
{
    if (*(uint64_t*)a == *(uint64_t*)b) return 0;
    return  *(uint64_t*)a < *(uint64_t*)b ? -1 : 1;
}

void printStatistics(int sender, int receiver, uint64_t* rawdata) {
    printf("%d,%d,", sender, receiver);
    qsort(rawdata, NUM_RUNS, sizeof(uint64_t), compare);
    uint64_t sum = 0;
    for (int i = 0; i < NUM_RUNS; i++)
        sum += rawdata[i];
    uint64_t avg = sum / NUM_RUNS;

    double stddev = 0;
    for (size_t i=0; i<NUM_RUNS; i++) {
        if (rawdata[i] > avg) {
            stddev += (rawdata[i] - avg) * (rawdata[i] - avg);
        } else {
            stddev += (avg- rawdata[i]) * (avg - rawdata[i]);
        }
    }
    stddev /= NUM_RUNS;
    stddev = sqrt(stddev);

    // count,avg,stddev,median,min,max
    printf("%d,%lu,%f,%lu,%lu,%lu\n", NUM_RUNS, avg, stddev,
            rawdata[NUM_RUNS / 2], rawdata[0],rawdata[NUM_RUNS-1]);
}

void split(const std::string &s, char delim, std::vector<std::string> &elems) {
    std::stringstream ss;
    ss.str(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        elems.push_back(item);
    }
}
std::vector<std::string> split(const std::string &s, char delim) {
    std::vector<std::string> elems;
    split(s, delim, elems);
    return elems;
}

std::vector<int> parseCores(const char* coreDesc) {
    std::vector<int> cores;
    std::vector<std::string> ranges = split(coreDesc, ',');
    for (int i = 0; i < ranges.size(); i++) {
        if (ranges[i].find("-") == std::string::npos)
            cores.push_back(atoi(ranges[i].c_str()));
        else {
            auto bounds = split(ranges[i], '-');
            for (int k = atoi(bounds[0].c_str()); k <= atoi(bounds[1].c_str());
                    k++)
                cores.push_back(k);
        }
    }
    return cores;
}
int main(int argc, const char** argv){
    // Read arguments
    if (argc < 2) {
        printf("Usage: ./CCCC <cpu_range>\n");
        return 1;
    }

    std::vector<int> cores = parseCores(argv[1]);
    std::atomic<uint64_t> timestamp;
    uint64_t *rawdata;
    for (int i = 0; i < cores.size(); i++) {
        for (int k = 0; k < cores.size(); k++) {
            if (cores[i] == cores[k]) continue;
            timestamp = 1;
            std::thread r(recv, cores[k], &timestamp, &rawdata);
            send(cores[i], &timestamp);
            r.join();
            // Analyze data
            PerfUtils::Util::serialize();
            printStatistics(cores[i], cores[k], rawdata);
            free(rawdata);
            PerfUtils::Util::serialize();
        }
    }
}
