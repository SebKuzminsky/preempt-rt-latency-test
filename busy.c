#define _GNU_SOURCE

#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/param.h>
#include <sys/resource.h>


uint64_t get_freq(void) {
#if defined(__i386__) || defined(__amd64__)
    // must have `constant_tsc` in /proc/cpuinfo
    // cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq (that's in kHz, return *1000)
    return 4500000000;

#elif defined(__aarch64__)
    uint32_t freq;
    asm volatile("mrs %0, cntfrq_el0" : "=r" (freq));
    return freq;

#else
#error "don't know how to read cycle counter on unknown arch (supported: i386, amd64, and arm64)"
#endif
}

uint64_t get_clocks(void) {
#if defined(__i386__) || defined(__amd64__)
    return __builtin_ia32_rdtsc();

#elif defined(__aarch64__)
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r" (val));
    return val;

#else
#error "don't know how to read cycle counter on unknown arch (supported: i386, amd64, and arm64)"
#endif
}


// Busywait reading the hardware cycle counter until one millisecond
// has passed.
void busywait_1ms(void) {
    uint64_t freq = get_freq();
    uint64_t clocks_when_done = get_clocks() + (freq/1000);
    do {
    } while (get_clocks() < clocks_when_done);
}


// Busywait 1 ms, 10,000 times (10 seconds), and keep track of the max
// amount of cycles actually spent on each iteration.
//
// If the thread has exclusive use of the CPU, the max time per iteration
// should be very close to 1 ms.
//
// If the max time per iteration is significantly higher than 1 ms it
// indicates that the thread got preempted or otherwise did not actually
// get exclusive use of the CPU.

void * rt_func(void * unused) {
    uint64_t freq = get_freq();
    int iteration_counter = 0;
    uint64_t prev_clocks;
    uint64_t delta_min = UINT64_MAX, delta_max = 0;
    uint64_t total_duration = 0;

    // Fetch & report actual thread scheduling attributes.
    {
        int r;
        pthread_attr_t attr;

        r = pthread_getattr_np(pthread_self(), &attr);
        if (r != 0) {
            printf("failed to get pthread attr: %s\n", strerror(r));
        }

        int policy;
        r = pthread_attr_getschedpolicy(&attr, &policy);
        if (r != 0) {
            printf("failed to get pthread attr: %s\n", strerror(r));
        }
        printf("scheduling policy: ");
        switch (policy) {
            case SCHED_FIFO:
                printf("SCHED_FIFO\n");
                break;
            case SCHED_OTHER:
                printf("SCHED_OTHER\n");
                break;
            default:
                printf("unknown (%d)\n", policy);
                break;
        }

        struct sched_param param;
        r = pthread_attr_getschedparam(&attr, &param);
        if (r != 0) {
            printf("failed to get pthread sched param: %s\n", strerror(r));
        }
        printf(
            "scheduling parameter priority: %d (min=%d, max=%d)\n",
            param.sched_priority,
            sched_get_priority_min(policy),
            sched_get_priority_max(policy)
        );

        cpu_set_t cpuset;
        r = sched_getaffinity(0 /* get cpu affinity of calling thread */, sizeof(cpu_set_t), &cpuset);
        if (r != 0) {
            printf("failed to get cpu affinity: %s\n", strerror(errno));
        }
        printf("cpu affinity:");
        int num_cpus = sysconf(_SC_NPROCESSORS_CONF);
        for (int i = 0; i < num_cpus; ++i) {
            if (CPU_ISSET(i, &cpuset)) {
                printf(" %d", i);
            }
        }
        printf("\n");
    }

    // Run the busywait loop 10k times, report some timing statistics.
    prev_clocks = get_clocks();
    do {
        busywait_1ms();

        // check the time
        uint64_t clocks = get_clocks();
        uint64_t delta = clocks - prev_clocks;
        total_duration += delta;
        if (delta > delta_max) delta_max = delta;
        if (delta < delta_min) delta_min = delta;
        prev_clocks = clocks;

        iteration_counter ++;
    } while(iteration_counter < 1e4);

    printf("after %d iterations:\n", iteration_counter);
    printf(
        "    min=%lu cycles (%.3f us, %.3f ms)\n",
        delta_min,
        1000000.0*((double)delta_min/(double)freq),
        1000.0*((double)delta_min/(double)freq)
    );
    printf(
        "    avg=%.3f cycles (%.3f us, %.3f ms)\n",
        (double)total_duration/(double)iteration_counter,
        1000000.0*(((double)total_duration/(double)iteration_counter)/(double)freq),
        1000.0*(((double)total_duration/(double)iteration_counter)/(double)freq)
    );
    printf(
        "    max=%lu cycles (%.3f us, %.3f ms)\n",
        delta_max,
        1000000.0*((double)delta_max/(double)freq),
        1000.0*((double)delta_max/(double)freq)
    );

    if (delta_max > 2*freq/1000) {
        printf("ERROR: worst latency > 2 ms\n\n");
    } else {
        printf("OK: worst latency < 2 ms\n\n");
    }

    return NULL;
}


void run_rt_task(int inherit_sched) {
    pthread_attr_t attr;

    if (pthread_attr_init(&attr) != 0) {
        printf("pthread_attr_init: %s\n", strerror(errno));
        exit(1);
    }

    // Set CPU affinity of the realtime thread to the last CPU in
    // the system.
    //
    // NOTE: this should match the kernel command line arguments
    // (nohz_full, isolcpus, rcu_nocbs, etc)
    int const rt_cpu_number = sysconf(_SC_NPROCESSORS_CONF) - 1;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(rt_cpu_number, &cpuset);
    if (pthread_attr_setaffinity_np(&attr, sizeof(cpuset), &cpuset) != 0) {
        printf("pthread_attr_setaffinity: %s\n", strerror(errno));
        exit(1);
    }

    if (inherit_sched == PTHREAD_INHERIT_SCHED) {
        printf("using PTHREAD_INHERIT_SCHED to keep SCHED_OTHER (with default priority)\n");
        if (pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED) != 0) {
            printf("pthread_attr_setinheritsched: %s\n", strerror(errno));
            exit(1);
        }
    } else {
        printf("using PTHREAD_EXPLICT_SCHED to set SCHED_FIFO (with highest priority)\n");

        if (pthread_attr_setschedpolicy(&attr, SCHED_FIFO) != 0) {
            printf("pthread_attr_setschedpolicy: %s\n", strerror(errno));
            exit(1);
        }

        // Pick a SCHED_FIFO priority well below the 50-99 where crucial
        // kernel threads run, to avoid starvation.
        struct sched_param param;
        memset(&param, 0, sizeof(param));
        param.sched_priority = MIN(5, sched_get_priority_max(SCHED_FIFO));

        if (pthread_attr_setschedparam(&attr, &param) != 0) {
            printf("pthread_attr_setschedparam: %s\n", strerror(errno));
            exit(1);
        }

        if (pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) != 0) {
            printf("pthread_attr_setinheritsched: %s\n", strerror(errno));
            exit(1);
        }
    }

    pthread_t rt_thread;
    if (pthread_create(&rt_thread, &attr, rt_func, NULL) != 0) {
        printf("pthread_create: %s\n", strerror(errno));
        exit(1);
    }

    pthread_join(rt_thread, NULL);
}


int main(int const argc, char const * const argv[]) {
    struct rlimit const unlimited = {RLIM_INFINITY, RLIM_INFINITY};

    // no limit to the realtime priority of this process
    if (setrlimit(RLIMIT_RTPRIO, &unlimited) != 0) {
        printf("setrlimit(RTLIMIT_RTPRIO): %s\n", strerror(errno));
        return 1;
    }

    // no limit to the amount of memory that can be mlocked by this process
    if (setrlimit(RLIMIT_MEMLOCK, &unlimited) != 0) {
        printf("setrlimit(RTLIMIT_MEMLOCK): %s\n", strerror(errno));
        return 1;
    }

    // immediately page in and lock all memory mapped by this process,
    // now and in the future
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        printf("mlockall(): %s\n", strerror(errno));
        return 1;
    }

    // don't trim freed memory from this process
    if (mallopt(M_TRIM_THRESHOLD, -1) != 1) {
        printf("mallopt(M_TRIM_THRESHOLD, -1) failed\n");
        return 1;
    }

    // map and then free (but don't trim) a bunch of pages, so they're
    // instantly available in case we'll use them later
    void * buf = malloc(32*1024*1024);
    if (buf == NULL) {
        printf("malloc() failed\n");
        return 1;
    }
    free(buf);

    run_rt_task(PTHREAD_INHERIT_SCHED);
    run_rt_task(PTHREAD_EXPLICIT_SCHED);

    return 0;
}
