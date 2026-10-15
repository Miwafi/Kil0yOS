/* Phase 1.3 acceptance: REAL multithreading through musl pthreads.
 * Exercises clone(CLONE_VM|THREAD|FILES|SIGHAND|SETTLS|PARENT_SETTID|
 * CHILD_CLEARTID), the futex WAIT/WAKE rewrite, TLS (FS base) switching
 * and exit_group. Must print the counters, join all workers and exit 0
 * - a broken clone/futex/scheduler shows up as a hang or a fault. */
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>

#define NTHREADS 4

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int counter = 0;
static int done[NTHREADS];

static void* worker(void* arg) {
    long id = (long)arg;
    printf("thr: worker %ld start\n", id);
    for (int i = 0; i < 25; i++) {
        pthread_mutex_lock(&mtx);
        counter++;
        pthread_mutex_unlock(&mtx);
        usleep(1000);
    }
    pthread_mutex_lock(&mtx);
    done[id] = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mtx);
    return (void*)(id + 1);   /* distinct return value per thread */
}

int main(void) {
    pthread_t th[NTHREADS];
    printf("thr: spawning %d workers\n", NTHREADS);
    for (long i = 0; i < NTHREADS; i++) {
        if (pthread_create(&th[i], NULL, worker, (void*)i) != 0) {
            printf("thr: create %ld FAILED\n", i);
            return 1;
        }
    }

    /* Wait for every worker flag via the condvar (futex WAIT/WAKE
     * across genuinely concurrent threads) */
    for (int left = NTHREADS; left > 0;) {
        pthread_mutex_lock(&mtx);
        left = 0;
        for (int i = 0; i < NTHREADS; i++)
            if (!done[i]) left++;
        if (left)
            pthread_cond_wait(&cv, &mtx);
        pthread_mutex_unlock(&mtx);
    }

    for (int i = 0; i < NTHREADS; i++) {
        void* ret = NULL;
        if (pthread_join(th[i], &ret) != 0) {
            printf("thr: join %d FAILED\n", i);
            return 2;
        }
        printf("thr: joined %d -> %ld\n", i, (long)ret);
    }

    printf("thr: counter=%d (want %d)\n", counter, NTHREADS * 25);
    fflush(stdout);
    if (counter != NTHREADS * 25) return 3;
    printf("thr: OK\n");
    return 0;
}
