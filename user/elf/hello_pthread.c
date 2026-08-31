/* Phase 3.2 acceptance: dynamically-linked glibc program using pthread
 * primitives (mutex + condition variable on the main thread). Exercises
 * the futex/rseq/set_robust_list stubs; must terminate, never deadlock. */
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

int main(void) {
    printf("pt: lock\n");
    pthread_mutex_lock(&mtx);
    printf("pt: locked rseq registered=%d\n", 0);
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mtx);
    printf("pt: unlocked, done\n");
    fflush(stdout);
    return 7;
}
