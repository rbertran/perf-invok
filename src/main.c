#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <limits.h>
#include <assert.h>

#include "breakpoint.h"
#include "sample.h"

#define MAX_SAMPLES 8192

int pid;
Sample samples[MAX_SAMPLES];
unsigned int sampleCount = 0;
unsigned int flushedSampleCount = 0;
int printHeaders = 1;
int sampleInProgress = 1;
FILE *outputFile;

void handler(int signum) {
    kill(pid, signum);

    if (sampleInProgress) {
        endSample(&samples[sampleCount - flushedSampleCount]);
        sampleCount++;
    }

    printSamples(outputFile, sampleCount - flushedSampleCount, samples, printHeaders);

    if (outputFile != stderr) fclose(outputFile);
    exit(-1);
}

int perInvocationPerformance(unsigned long long addrStart,
                              unsigned long long addrEnd,
                              unsigned int maxSamples,
                              FILE *outputFile) {
    int status;
    Breakpoint bp;

    setBreakpoint(pid, addrStart, &bp);
    ptrace(PTRACE_CONT, pid, 0, 0);

    while (waitpid(pid, &status, 0) != -1 && sampleCount < maxSamples &&
           !WIFEXITED(status)) {
        resetBreakpoint(pid, &bp);
        setBreakpoint(pid, addrEnd, &bp);

        beginSample(&samples[sampleCount - flushedSampleCount]);
        sampleInProgress = 1;

        ptrace(PTRACE_CONT, pid, 0, 0);
        waitpid(pid, &status, 0);

        sampleInProgress = 0;
        endSample(&samples[sampleCount - flushedSampleCount]);

        sampleCount++;

        if (sampleCount % MAX_SAMPLES == 0) {
            printSamples(outputFile, sampleCount - flushedSampleCount,
                         samples, printHeaders);
            printHeaders = 0;
            flushedSampleCount += MAX_SAMPLES;
        }

        resetBreakpoint(pid, &bp);
        setBreakpoint(pid, addrStart, &bp);
        ptrace(PTRACE_CONT, pid, 0, 0);
    }

    if (sampleCount == maxSamples) kill(pid, SIGTERM);

    return 0;
}

void globalPerformance(unsigned int timeout) {
    int status;
    beginSample(&samples[0]);
    sampleInProgress = 1;

    ptrace(PTRACE_CONT, pid, 0, 0);
    alarm(timeout);
    waitpid(pid, &status, 0);

    sampleInProgress = 0;
    endSample(&samples[0]);
    sampleCount++;
}

int main(int argc, char **argv) {
    assert(argc >= 2);

    unsigned long long addrStart = 0;
    unsigned long long addrEnd = 0;
    unsigned int maxSamples = UINT_MAX;
    unsigned int programStart = 1;
    unsigned int timeout = 0;
    char *output = NULL;

    enum {
        EXPECTING_OPT, EXPECTING_ADDR_START, EXPECTING_ADDR_END,
        EXPECTING_MAX_SAMPLES, EXPECTING_PROGRAM, EXPECTING_OUTPUT,
        EXPECTING_TIMEOUT
    } state = EXPECTING_OPT;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        switch (state) {
            case EXPECTING_OPT:
                if (strcmp(arg, "-begin") == 0) state = EXPECTING_ADDR_START;
                else if (strcmp(arg, "-end") == 0) state = EXPECTING_ADDR_END;
                else if (strcmp(arg, "-max") == 0) state = EXPECTING_MAX_SAMPLES;
                else if (strcmp(arg, "-o") == 0) state = EXPECTING_OUTPUT;
                else if (strcmp(arg, "-timeout") == 0) state = EXPECTING_TIMEOUT;
                else {
                    state = EXPECTING_PROGRAM;
                    programStart = i;
                }
                break;
            case EXPECTING_ADDR_START:
                addrStart = strtoull(argv[i], NULL, 16);
                state = EXPECTING_OPT;
                break;
            case EXPECTING_ADDR_END:
                addrEnd = strtoull(argv[i], NULL, 16);
                state = EXPECTING_OPT;
                break;
            case EXPECTING_MAX_SAMPLES:
                maxSamples = atoi(argv[i]);
                state = EXPECTING_OPT;
                break;
            case EXPECTING_TIMEOUT:
                timeout = atoi(argv[i]);
                state = EXPECTING_OPT;
                break;
            case EXPECTING_OUTPUT:
                output = argv[i];
                state = EXPECTING_OPT;
                break;
            case EXPECTING_PROGRAM:
                break;
        }
    }

    printf("Executing ");
    for (int i = programStart; i < argc; i++) printf("%s ", argv[i]);
    printf("\n");

    pid = fork();

    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(1, &mask);
    sched_setaffinity(0, sizeof(cpu_set_t), &mask);

    if (pid == 0) {
        unsigned int numParams = argc - 3;
        char **newargs = malloc(sizeof(char*) * (numParams + 2));
        memcpy(newargs, &argv[programStart], sizeof(char*) * (numParams + 1));
        newargs[numParams + 1] = NULL;
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        execvp(argv[programStart], newargs);
    } else {
        outputFile = (output != NULL ? fopen(output, "w") : stderr);
        assert(outputFile != NULL);

        struct sigaction sa;
        sa.sa_handler = handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGKILL, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGALRM, &sa, NULL);

        int status;
        waitpid(pid, &status, 0); // Wait for child to start

        configureEvents(pid);

        if (addrStart > 0 && addrEnd > 0) {
            printf("Measuring performance counters from 0x%llx to 0x%llx (max. samples: %u).\n", addrStart, addrEnd, maxSamples);
            status = perInvocationPerformance(addrStart, addrEnd, maxSamples, outputFile);
        } else {
            printf("Measuring performance counters from global execution\n");
            globalPerformance(timeout);
        }

        printSamples(outputFile, sampleCount - flushedSampleCount, samples,
                     printHeaders);

        if (outputFile != stderr) fclose(outputFile);

        return status;
    }
}
