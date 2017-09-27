/*
  Copyright (C) 2017  Francesc Alted
  http://blosc.org
  License: BSD (see LICENSE.txt)

  Example program demonstrating how the different compression params affects
  the performance of root finding.

  To compile this program:

  $ gcc -O3 find_roots.c -o find_roots -lblosc

  To run:

  $ ./find_roots
  ...

*/

#include <stdio.h>
#include <time.h>
#include "blosc.h"

#if defined(_WIN32)
/* For QueryPerformanceCounter(), etc. */
    #include <windows.h>
#elif defined(__MACH__)
    #include <mach/clock.h>
    #include <mach/mach.h>
#elif defined(__unix__)
    #if defined(__linux__)
        #include <time.h>
      #else
        #include <sys/time.h>
      #endif
    #else
      #error Unable to detect platform.
#endif


#define KB  1024.
#define MB  (1024*KB)
#define GB  (1024*MB)


#define NCHUNKS 500
#define CHUNKSIZE (200 * 1000)  // fits well in modern L3 caches
#define NTHREADS 4

/* The type of timestamp used on this system. */
#define blosc_timestamp_t struct timespec

/* Set a timestamp value to the current time. */
void blosc_set_timestamp(blosc_timestamp_t* timestamp) {
#ifdef __MACH__ // OS X does not have clock_gettime, use clock_get_time
  clock_serv_t cclock;
  mach_timespec_t mts;
  host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
  clock_get_time(cclock, &mts);
  mach_port_deallocate(mach_task_self(), cclock);
  timestamp->tv_sec = mts.tv_sec;
  timestamp->tv_nsec = mts.tv_nsec;
#else
  clock_gettime(CLOCK_MONOTONIC, timestamp);
#endif
}

/* Given two timestamp values, return the difference in microseconds. */
double blosc_elapsed_usecs(blosc_timestamp_t start_time, blosc_timestamp_t end_time) {
    return (1e6 * (end_time.tv_sec - start_time.tv_sec))
           + (1e-3 * (end_time.tv_nsec - start_time.tv_nsec));
}

/* Given two timeval stamps, return the difference in seconds */
double getseconds(blosc_timestamp_t last, blosc_timestamp_t current) {
    return 1e-6 * blosc_elapsed_usecs(last, current);
}

/* Given two timeval stamps, return the time per chunk in usec */
double get_usec_chunk(blosc_timestamp_t last, blosc_timestamp_t current, int niter, size_t nchunks) {
    double elapsed_usecs = (double)blosc_elapsed_usecs(last, current);
    return elapsed_usecs / (double)(niter * nchunks);
}


void fill_buffer(double *x, int nchunk) {
    double incx = 10. / (NCHUNKS * CHUNKSIZE);

    for (int i = 0; i < CHUNKSIZE; i++) {
        x[i] = incx * (nchunk * CHUNKSIZE + i);
    }
}

void process_data(const double *x, double *y) {

    for (int i = 0; i < CHUNKSIZE; i++) {
        double xi = x[i];
        //y[i] = ((.25 * xi + .75) * xi - 1.5) * xi - 2;
        y[i] = (xi - 1.35) * (xi - 4.45) * (xi - 8.5);
    }
}

void find_root(const double *x, const double *y,
                      const double prev_value) {
    double pv = prev_value;
    int last_root_idx = -1;

    for (int i = 0; i < CHUNKSIZE; i++) {
        double yi = y[i];
        if (((yi > 0) - (yi < 0)) != ((pv > 0) - (pv < 0))) {
            if (last_root_idx != (i - 1)) {
                printf("%.16g, ", x[i]);
                last_root_idx = i;  // avoid the last point (ULP effects)
            }
        }
        pv = yi;
    }
}


int compute_vectors(void) {
    static double buffer_x[CHUNKSIZE];
    static double buffer_y[CHUNKSIZE];
    const size_t isize = CHUNKSIZE * sizeof(double);
    int dsize;
    long nbytes = 0;
    blosc2_cparams cparams = BLOSC_CPARAMS_DEFAULTS;
    blosc2_dparams dparams = BLOSC_DPARAMS_DEFAULTS;
    blosc2_schunk *sc_x, *sc_y;
    int nchunk;
    blosc_timestamp_t last, current;
    double ttotal;
    double prev_value;

    /* Create a super-chunk container for input (X values) */
    cparams.typesize = sizeof(double);
    cparams.compcode = BLOSC_BLOSCLZ;
    cparams.clevel = 5;
    cparams.filters[0] = BLOSC_TRUNC_PREC;
    cparams.filters_meta[0] = 23;  // treat doubles as floats
    cparams.nthreads = NTHREADS;
    dparams.nthreads = NTHREADS;
    sc_x = blosc2_new_schunk(cparams, dparams);

    /* Create a super-chunk container for output (Y values) */
    sc_y = blosc2_new_schunk(cparams, dparams);

    /* Now fill the buffer with even values between 0 and 10 */
    blosc_set_timestamp(&last);
    for (nchunk = 0; nchunk < NCHUNKS; nchunk++) {
        fill_buffer(buffer_x, nchunk);
        blosc2_append_buffer(sc_x, isize, buffer_x);
        nbytes += isize;
    }
    blosc_set_timestamp(&current);
    ttotal = getseconds(last, current);
    printf("Creation time for X values: %.3g s, %.1f MB/s\n",
           ttotal, nbytes / (ttotal * MB));
    printf("Compression for X values: %.1f MB -> %.1f MB (%.1fx)\n",
           sc_x->nbytes / MB, sc_x->cbytes / MB,
           (1. * sc_x->nbytes) / sc_x->cbytes);

    /* Retrieve the chunks and compute the polynomial in another super-chunk */
    blosc_set_timestamp(&last);
    for (nchunk = 0; nchunk < NCHUNKS; nchunk++) {
        dsize = blosc2_decompress_chunk(sc_x, nchunk, (void *) buffer_x, isize);
        if (dsize < 0) {
            printf("Decompression error.  Error code: %d\n", dsize);
            return dsize;
        }
        process_data(buffer_x, buffer_y);
        blosc2_append_buffer(sc_y, isize, buffer_y);
    }
    blosc_set_timestamp(&current);
    ttotal = getseconds(last, current);
    printf("Computing Y polynomial: %.3g s, %.1f MB/s\n",
           ttotal,
           2. * nbytes / (ttotal * MB));    // 2 super-chunks involved
    printf("Compression for Y values: %.1f MB -> %.1f MB (%.1fx)\n",
           sc_y->nbytes / MB, sc_y->cbytes / MB,
           (1. * sc_y->nbytes) / sc_y->cbytes);

    /* Find the roots of the polynomial */
    printf("Roots found at: ");
    blosc_set_timestamp(&last);
    prev_value = buffer_y[0];
    for (nchunk = 0; nchunk < NCHUNKS; nchunk++) {
        dsize = blosc2_decompress_chunk(sc_y, nchunk, (void *) buffer_y, isize);
        if (dsize < 0) {
            printf("Decompression error.  Error code: %d\n", dsize);
            return dsize;
        }
        dsize = blosc2_decompress_chunk(sc_x, nchunk, (void *) buffer_x, isize);
        if (dsize < 0) {
            printf("Decompression error.  Error code: %d\n", dsize);
            return dsize;
        }
        find_root(buffer_x, buffer_y, prev_value);
        prev_value = buffer_y[CHUNKSIZE - 1];
    }
    blosc_set_timestamp(&current);
    ttotal = getseconds(last, current);
    printf("\n");
    printf("Find root time:  %.3g s, %.1f MB/s\n",
           ttotal, 2. * nbytes / (ttotal * MB));    // 2 super-chunks involved

    /* Free resources */
    /* Destroy the super-chunk */
    blosc2_destroy_schunk(sc_x);
    blosc2_destroy_schunk(sc_y);
    return 0;
}


int main() {
    printf("Blosc version info: %s (%s)\n",
           BLOSC_VERSION_STRING, BLOSC_VERSION_DATE);

    /* Initialize the Blosc compressor */
    blosc_init();

    compute_vectors();

    /* Destroy the Blosc environment */
    blosc_destroy();

    return 0;
}
