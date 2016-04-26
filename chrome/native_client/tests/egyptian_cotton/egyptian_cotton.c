/*
 * Copyright 2010 The Native Client Authors.  All rights reserved.
 * Use of this source code is governed by a BSD-style license that can
 * be found in the LICENSE file.
 */

/*
 * Egyptian Cotton:  maximize the thread count.
 */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char const *short_usage =
    ("Usage: egytian_cotton [-fhv] [-n num_threads] [-s bytes]\n");
static char const *flags_usage =
    ("       -f  expect thread creation to fail for given\n"
     "           thread creation parameters\n"
     "       -h  longer help\n"
     "       -v  increase verbosity\n"
     "       -n  number of additional threads to create\n"
     "       -s  stack size for each new thread\n");
static char const *long_winded_explanation[] = {
  "This is a test to exercise the thread library when used",
  "in a high thread-count situation.  The exit status (and",
  "SUCCESS/FAIL output) will indicate success when all requested",
  "threads were created without error and the -f flag is not",
  "specified.  This is typicaly used to make sure that a large",
  "number of threads can be created -- as long as the stack size",
  "requested is moderate, so we don't run out of memory",
  "Conversely, the -f flag is used to ensure that the thread",
  "library returns the appropriate error rather than faulting,",
  "when a larger-than-supported number of threads is requested,",
  "or if the total amount of stack memory exceeds available",
  "memory / address space",
  NULL,
};

#define DEFAULT_NUM_THREADS   ((size_t) 4095)
#define DEFAULT_STACK_SIZE    ((size_t) 1024)

#if defined(__native_client__)
# define PRIdS  "d"
# define PRIxS  "x"
#else
# define PRIdS  "zd"
# define PRIxS  "zx"
#endif

int             gVerbosity = 0;
int             gExpectThreadCreationFailure = 0;

size_t          kNewlineMask = 0x3f;
size_t          kCountMask = 0xff;

pthread_mutex_t gBarrierMu = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  gBarrierCv = PTHREAD_COND_INITIALIZER;
int             gThreadWait = 1;

void *thread_start(void *state) {
  int tid = (int) (uintptr_t) state;

  pthread_mutex_lock(&gBarrierMu);
  while (gThreadWait) {
    if (gVerbosity > 1) {
      printf("Thread %d: waiting\n", tid);
    }
    pthread_cond_wait(&gBarrierCv, &gBarrierMu);
  }
  pthread_mutex_unlock(&gBarrierMu);
  return NULL;
}

int main(int ac,
         char **av) {
  int             opt;
  size_t          num_threads = DEFAULT_NUM_THREADS;
  size_t          stack_size = DEFAULT_STACK_SIZE;
  char const      **line;
  int             rv = 0;
  pthread_t       thr_state;
  pthread_attr_t  attr;
  size_t          tid;

  while (EOF != (opt = getopt(ac, av, "fhn:s:v"))) {
    switch (opt) {
      case 'f':
        gExpectThreadCreationFailure = 1;
        break;
      case 'n':
        num_threads = strtoul(optarg, (char **) NULL, 0);
        break;
      case 's':
        stack_size = strtoul(optarg, (char **) NULL, 0);
        break;
      case 'v':
        ++gVerbosity;
        break;
      case 'h':
        fprintf(stderr, "%s%s\n", short_usage, flags_usage);
        for (line = long_winded_explanation; NULL != *line; ++line) {
          fprintf(stderr, "       %s\n", *line);
        }
        return 0;
      default:
        fprintf(stderr, "%s\n", short_usage);
        return 1;
    }
  }

  if (gVerbosity > 1) {
    printf("Creating %"PRIdS" threads with 0x%"PRIxS" byte stacks\n",
           num_threads,
           stack_size);
    printf("According to the command line parameters,"
           " this is%s expected to work.\n",
           gExpectThreadCreationFailure ? " not" : "");
  }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, stack_size);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  for (tid = 0; tid < num_threads; ++tid) {
    if (gVerbosity > 0) {
      printf("Creating thread %"PRIdS"\n", tid); fflush(stdout);
    }
    /*
     * thr_state is overwritten each time.
     */
    if (0 != (rv = pthread_create(&thr_state,
                                  &attr,
                                  thread_start,
                                  (void *) tid))) {
      printf("Thread creation failed at thread %"PRIdS", error %d (%s)\n",
             tid, rv, strerror(rv));
      if (gExpectThreadCreationFailure) {
        if (EAGAIN == rv) {
          printf("This is expected.\n");
          rv = 0;
          goto cleanup_exit;
        }
        /*
         * Anything other than EAGAIN is bad.
         */
      }
      break;
    }
    putchar('.');
    if (kCountMask == (tid & kCountMask)) {
      printf("%"PRIdS, tid);
    }
    if (kNewlineMask == (tid & kNewlineMask)) {
      putchar('\n');
    }
  }
  if (0 == rv && gExpectThreadCreationFailure) {
    printf("Expected thread creation failure.\n");
    rv = 1;
  }
cleanup_exit:
  pthread_mutex_lock(&gBarrierMu);
  gThreadWait = 0;
  pthread_cond_broadcast(&gBarrierCv);
  pthread_mutex_unlock(&gBarrierMu);

  if (0 == rv) {
    printf("PASSED\n");
  } else {
    printf("FAILED\n");
  }

  return rv;
}
