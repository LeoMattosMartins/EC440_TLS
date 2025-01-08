#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#define PAGE_SIZE 4096
#define MAX_THREADS 128

typedef struct tls
{
        void** pages;
        int* refCount;
        pthread_t tid;
        size_t size, numPages;
} tls_t;

static tls_t *tlsMap[MAX_THREADS] = {NULL};
static __thread tls_t *tlsCurrent = NULL;
static pthread_mutex_t tlsLock = PTHREAD_MUTEX_INITIALIZER;

void tls_handle_segfault(int sig, siginfo_t *si, void *context)
{
        void *faultAddr = si->si_addr;

        pthread_mutex_lock(&tlsLock);

        if( tlsCurrent )
        {
                size_t ii = 0;

                for( ii = 0; ii < tlsCurrent->numPages; ii++ )
                {
                        if( (faultAddr >= tlsCurrent->pages[ii]) && (faultAddr < tlsCurrent->pages[ii] + PAGE_SIZE) )
                        {
                                pthread_mutex_unlock(&tlsLock);
                                pthread_exit(NULL);
                        }
                }
        }

        pthread_mutex_unlock(&tlsLock);

        signal(SIGSEGV, SIG_DFL);
        raise(SIGSEGV);
}

void initialize_signal_handler()
{
        static int initialized = 0;
        if( !initialized )
        {
                struct sigaction sa;
                sa.sa_flags = SA_SIGINFO;
                sa.sa_sigaction = tls_handle_segfault;
                sigaction(SIGSEGV, &sa, NULL);
                initialized = 1;
        }
}

int find_tls_index(pthread_t tid)
{
        int ii = 0;

        for( ii = 0; ii < MAX_THREADS; ii++ )
        {
                if (tlsMap[ii] && pthread_equal(tlsMap[ii]->tid, tid)) { return ii; }
        }

        return -1;
}

int tls_create(unsigned int size)
{
        pthread_mutex_lock(&tlsLock);

        initialize_signal_handler();

        if( tlsCurrent )
        {
                pthread_mutex_unlock(&tlsLock);
                return -1;
        }

        size_t numPages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        void **pages = malloc(numPages * sizeof(void*));
        int *refCount = malloc(numPages * sizeof(int));

        if( !pages || !refCount )
        {
                free(pages);
                free(refCount);
                pthread_mutex_unlock(&tlsLock);
                return -1;
        }

        size_t ii = 0;

        for( ii = 0; ii < numPages; ii++)
        {
                pages[ii] = mmap(NULL, PAGE_SIZE, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

                if( pages[ii] == MAP_FAILED )
                {
                        size_t jj = 0;

                        for( jj = 0; jj < ii; jj++ ) { munmap(pages[jj], PAGE_SIZE); }

                        free(pages);
                        free(refCount);
                        pthread_mutex_unlock(&tlsLock);

                        return -1;
                }

                refCount[ii] = 1;
        }

        tlsCurrent = malloc(sizeof(tls_t));

        if( !tlsCurrent )
        {
                size_t jj = 0;

                for( jj = 0; jj < numPages; jj++ ) { munmap(pages[jj], PAGE_SIZE); }

                free(pages);
                free(refCount);
                pthread_mutex_unlock(&tlsLock);

                return -1;
        }

        tlsCurrent->tid = pthread_self();
        tlsCurrent->size = size;
        tlsCurrent->numPages = numPages;
        tlsCurrent->pages = pages;
        tlsCurrent->refCount = refCount;

        int jj = 0;

        for( jj = 0; jj < MAX_THREADS; jj++ )
        {
                if( !tlsMap[jj] )
                {
                        tlsMap[jj] = tlsCurrent;
                        break;
                }
        }

        pthread_mutex_unlock(&tlsLock);

        return 0;
}

int tls_clone(pthread_t tid)
{
        pthread_mutex_lock(&tlsLock);

        if( tlsCurrent )
        {
                pthread_mutex_unlock(&tlsLock);
                return -1;
        }

        int targetIndex = find_tls_index(tid);

        if (targetIndex == -1)
        {
                pthread_mutex_unlock(&tlsLock);
                return -1;
        }

        tls_t *tlsTarget = tlsMap[targetIndex];

        tlsCurrent = malloc(sizeof(tls_t));

        if( !tlsCurrent )
        {
                pthread_mutex_unlock(&tlsLock);
                return -1;
        }

        tlsCurrent->tid = pthread_self();
        tlsCurrent->size = tlsTarget->size;
        tlsCurrent->numPages = tlsTarget->numPages;
        tlsCurrent->pages = malloc(tlsCurrent->numPages * sizeof(void*));
        tlsCurrent->refCount = tlsTarget->refCount;

        if( !tlsCurrent->pages || !tlsCurrent->refCount )
        {
                free(tlsCurrent->pages);
                free(tlsCurrent->refCount);
                free(tlsCurrent);
                pthread_mutex_unlock(&tlsLock);
                return -1;
        }

        size_t ii = 0;

        for( ii = 0; ii < tlsCurrent->numPages; ii++ )
        {
                tlsCurrent->pages[ii] = tlsTarget->pages[ii];
                tlsTarget->refCount[ii]++;
                tlsCurrent->refCount[ii] = tlsTarget->refCount[ii];
        }

        int jj = 0;

        for( jj = 0; jj < MAX_THREADS; jj++ )
        {
                if( !tlsMap[jj] )
                {
                        tlsMap[jj] = tlsCurrent;
                        break;
                }
        }

        pthread_mutex_unlock(&tlsLock);

        return 0;
}

int tls_destroy()
{
        pthread_mutex_lock(&tlsLock);

        if( !tlsCurrent )
        {
                pthread_mutex_unlock(&tlsLock);
                return -1;
        }

        size_t ii = 0;

        for( ii = 0; ii < tlsCurrent->numPages; ii++ )
        {
                if( --tlsCurrent->refCount[ii] == 0 ) { munmap(tlsCurrent->pages[ii], PAGE_SIZE); }
        }

        int jj = 0;

        for( jj = 0; jj < MAX_THREADS; jj++ )
        {
                if( tlsMap[jj] == tlsCurrent )
                {
                        tlsMap[jj] = NULL;
                        break;
                }
        }

        free(tlsCurrent->pages);
        free(tlsCurrent->refCount);
        tlsCurrent = NULL;

        pthread_mutex_unlock(&tlsLock);

        return 0;
}

int tls_write(unsigned int offset, unsigned int length, char *buffer)
{
        pthread_mutex_lock(&tlsLock);

        if( !tlsCurrent || offset + length > tlsCurrent->size )
        {
                pthread_mutex_unlock(&tlsLock);
                return -1;
        }

        int ii = 0;

        for( ii = 0; ii < length; ii++ )
        {
                size_t pageIndex = (offset + ii) / PAGE_SIZE;
                size_t pageOffset = (offset + ii) % PAGE_SIZE;

                if( tlsCurrent->refCount[pageIndex] > 1 )
                {
                        void *new_page = mmap(NULL, PAGE_SIZE, PROT_WRITE | PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

                        if( new_page == MAP_FAILED )
                        {
                                pthread_mutex_unlock(&tlsLock);
                                return -1;
                        }

                        memcpy(new_page, tlsCurrent->pages[pageIndex], PAGE_SIZE);
                        /* munmap(tlsCurrent->pages[pageIndex], PAGE_SIZE); */

                        tlsCurrent->refCount[pageIndex]--;
                        tlsCurrent->pages[pageIndex] = new_page;
                        tlsCurrent->refCount[pageIndex] = 1;
                }

                mprotect(tlsCurrent->pages[pageIndex], PAGE_SIZE, PROT_WRITE);
                ((char *)tlsCurrent->pages[pageIndex])[pageOffset] = buffer[ii];
                mprotect(tlsCurrent->pages[pageIndex], PAGE_SIZE, PROT_NONE);
        }

        pthread_mutex_unlock(&tlsLock);

        return 0;
}

int tls_read(unsigned int offset, unsigned int length, char *buffer)
{
        pthread_mutex_lock(&tlsLock);

        if( !tlsCurrent || offset + length > tlsCurrent->size )
        {
                pthread_mutex_unlock(&tlsLock);
                return -1;
        }

        int ii = 0;

        for( ii = 0; ii < length; ii++ )
        {
                size_t pageIndex = (offset + ii) / PAGE_SIZE;
                size_t pageOffset = (offset + ii) % PAGE_SIZE;

                mprotect(tlsCurrent->pages[pageIndex], PAGE_SIZE, PROT_READ);
                buffer[ii] = ((char *)tlsCurrent->pages[pageIndex])[pageOffset];
                mprotect(tlsCurrent->pages[pageIndex], PAGE_SIZE, PROT_NONE);
        }

        pthread_mutex_unlock(&tlsLock);
        return 0;
}
