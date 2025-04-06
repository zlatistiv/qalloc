#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <assert.h>

#define INITIAL_SIZE 256 // This is the initial size in pages
#define EXTEND_MIN 16 // This is the minimum sbrk() increment size in pages
#define ALIGNMENT 16 // The byte alignment returned by a call to malloc

typedef struct chunk_t chunk_t;

struct chunk_t {
	size_t size;
	chunk_t *next;
	chunk_t *prev;
	bool free;
};

static chunk_t *first_chunk = NULL;
static chunk_t *last_chunk = NULL;
static size_t pagesize;
static pthread_mutex_t mutex;


static void print_chunk(chunk_t *chunk);
static void print_heap();
static void debug_heap();
static chunk_t *extend(size_t);
static chunk_t *best_fit(size_t, size_t);
static void crop(chunk_t *, size_t);
static void *alloc(size_t, size_t);
void *malloc(size_t);
void free(void *);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void *reallocarray(void *, size_t, size_t);
size_t malloc_usable_size(void *);
void *aligned_alloc(size_t, size_t);
int posix_memalign(void **, size_t, size_t);


static void print_chunk(chunk_t *chunk) {
	fprintf(stderr, "Addr: %p\n", chunk);
	fprintf(stderr, "Size: %d\n", chunk->size);
	fprintf(stderr, "Next: %p\n", chunk->next);
	fprintf(stderr, "Prev: %p\n", chunk->prev);
	fprintf(stderr, "Free: %b\n", chunk->free);
	if (chunk->next) fprintf(stderr, "Data: %d bytes\n", (size_t)((void *)chunk->next - (void *)chunk) - sizeof(chunk_t));
	else fprintf(stderr, "Data: %d bytes\n", (size_t)(sbrk(0) - (void *)chunk) - sizeof(chunk_t));
	fprintf(stderr, "\n");
}

static void print_heap() {
	for (chunk_t *chunk = first_chunk; chunk != NULL; chunk = chunk->next) print_chunk(chunk);
}

// Walk the chunks and look for discrepancies between size and pointers
static void debug_heap() {
	for (chunk_t *chunk = first_chunk; chunk->next != NULL; chunk = chunk->next) {
		if ((void *)chunk + chunk->size + sizeof(chunk_t) != (void *)chunk->next) {
			fprintf(stderr, "Error, misaligned chunk and chunk->next!\n");
			print_chunk(chunk);
			print_chunk(chunk->next);
			fprintf(stderr, "First chunk: %p\n", first_chunk);
			fprintf(stderr, "Last chunk: %p\n", last_chunk);
			exit(EXIT_FAILURE);
		}
	}
	for (chunk_t *chunk = last_chunk; chunk != first_chunk; chunk = chunk->prev) {
		if ((void *)chunk - chunk->prev->size - sizeof(chunk_t) != (void *)chunk->prev) {
			fprintf(stderr, "Error, misaligned chunk and chunk->prev!\n");
			print_chunk(chunk);
			print_chunk(chunk->prev);
			fprintf(stderr, "First chunk: %p\n", first_chunk);
			fprintf(stderr, "Last chunk: %p\n", last_chunk);
			exit(EXIT_FAILURE);
		}
	}
}


static void init() {
	pagesize = sysconf(_SC_PAGESIZE);
	size_t size;
	void *start;
	void *end;

	pthread_mutexattr_t attr;
	if (pthread_mutexattr_init(&attr) != 0) {
		perror("pthread_mutexattr_init");
		exit(EXIT_FAILURE);
	}
	if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL) != 0) {
		perror("pthread_mutexattr_settype");
		exit(EXIT_FAILURE);
	}
	if (pthread_mutex_init(&mutex, &attr));

	size = pagesize * INITIAL_SIZE;

	if ((start = (sbrk(size))) == (void *)-1) exit(EXIT_FAILURE);
	end = start + size;
	assert(end == sbrk(0));

	first_chunk = (chunk_t *)start;
	first_chunk->size = size - 2 * sizeof(chunk_t);
	first_chunk->next = (chunk_t *)(end - sizeof(chunk_t));
	first_chunk->prev = NULL;
	first_chunk->free = 1;
	
	last_chunk = first_chunk->next;
	last_chunk->size = 0;
	last_chunk->next = NULL;
	last_chunk->prev = first_chunk;
	last_chunk->free = 0;
}

static chunk_t *extend(size_t size) {
	if (size < EXTEND_MIN * pagesize) size = EXTEND_MIN * pagesize;
	size_t size_aligned = (size + sizeof(chunk_t) + pagesize - 1) & ~(pagesize - 1); // Align the size
	
	if (sbrk(size_aligned) == (void *)(-1)) return NULL;

	last_chunk->size = size_aligned - sizeof(chunk_t);
	last_chunk->next = (chunk_t *)((void *)last_chunk + last_chunk->size + sizeof(chunk_t));
	last_chunk->free = 1;
	last_chunk->next->prev = last_chunk;
	last_chunk = last_chunk->next;

	last_chunk->size = 0;
	last_chunk->next = NULL;
	last_chunk->free = 0;

	return last_chunk->prev;
}

// Return the smallest fitting chunk
static chunk_t *best_fit(size_t alignment, size_t size) {
	chunk_t *best = NULL;
	for (chunk_t *chunk = first_chunk; chunk != NULL; chunk = chunk->next) {
		if (chunk->size >= size && chunk->free && ((uintptr_t)chunk + sizeof(chunk_t)) % alignment == 0) {
			if (!best) best = chunk;
			else if (chunk->size < best->size) best = chunk;
		}
	}
	return best;
}

// Crop the chunk to size.
// A new free chunk is created out of the leftover space.
// Passed size should be aligned to ALIGNMENT!
static void crop(chunk_t *chunk, size_t size) {
	ptrdiff_t leftover = chunk->size - size - sizeof(chunk_t);

	if (leftover >= ALIGNMENT) {
		chunk_t *new = (chunk_t *)((void *)chunk + sizeof(chunk_t) + size);
		
		new->size = leftover;
		new->next = chunk->next;
		new->prev = chunk;
		new->free = 1;

		chunk->size = size;
		chunk->next = new;

		new->next->prev = new;
	}
}

static void *alloc(size_t alignment, size_t size) {
	size = (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1); // Align the size to ALIGNMENT bytes
	if (size > PTRDIFF_MAX) return NULL;

	pthread_mutex_lock(&mutex);
	if (!first_chunk) init();

	chunk_t *chunk;
	void *addr = NULL;

	chunk = best_fit(alignment, size);
	if (!chunk) chunk = extend(size);
	
	if (chunk) {
		chunk->free = 0;
		crop(chunk, size);
		addr = (void *)chunk + sizeof(chunk_t);
	}
	
	pthread_mutex_unlock(&mutex);
	return addr;
}

void *malloc(size_t size) {
	void *addr = alloc(ALIGNMENT, size);
#ifdef DEBUG
	fprintf(stderr, "malloc(%d)", size);
	fprintf(stderr, " = %p\n", addr);
#endif
	return addr;
}


void free(void *ptr) {
#ifdef DEBUG
	fprintf(stderr, "free(%p)\n", ptr);
#endif
	if (!ptr) return;
	pthread_mutex_lock(&mutex);

	chunk_t *chunk = (chunk_t *)(ptr - sizeof(chunk_t));
	chunk->free = 1;
	
	// Coalesce to the left
	if (chunk->prev && chunk->prev->free) {
		chunk->prev->size += chunk->size + sizeof(chunk_t);
		chunk->prev->next = chunk->next;
		chunk->next->prev = chunk->prev;
		chunk = chunk->prev;
	}
	// Coalesce to the right
	if (chunk->next->free) {
		chunk->size += chunk->next->size + sizeof(chunk_t);
		chunk->next = chunk->next->next;
		chunk->next->prev = chunk;
	}
	pthread_mutex_unlock(&mutex);
}

void *calloc(size_t nmemb, size_t size) {
#ifdef DEBUG
	fprintf(stderr, "calloc(%d, %d)", nmemb, size);
#endif
	void *addr;

	if (size > SIZE_MAX / nmemb) return NULL;
	size *= nmemb;
	addr = malloc(size);
	if (addr) memset(addr, 0, size);
#ifdef DEBUG	
	fprintf(stderr, " = %p\n", addr);
#endif
	return addr;
}

void *realloc(void *ptr, size_t size) {
#ifdef DEBUG
	fprintf(stderr, "realloc(%p, %d)", ptr, size);
#endif
	if (!ptr) return malloc(size);
	if (!size) {
		free(ptr);
		return NULL;
	}

	size = (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1); // Align the size to ALIGNMENT bytes
	chunk_t *chunk = (chunk_t *)(ptr - sizeof(chunk_t));
	ptrdiff_t delta = size - chunk->size;

	if (delta <= 0) {
		pthread_mutex_lock(&mutex);
		crop(chunk, size);
		pthread_mutex_unlock(&mutex);
	}
	else {
		pthread_mutex_lock(&mutex);
		if (chunk->next->free && delta <= chunk->next->size + sizeof(chunk_t)) { // The next chunk is open for business
			chunk->size += chunk->next->size + sizeof(chunk_t);
			chunk->next = chunk->next->next;
			chunk->next->prev = chunk;
			crop(chunk, size);
			pthread_mutex_unlock(&mutex);
		}
		else {
			pthread_mutex_unlock(&mutex);
			void *addr = malloc(size);
			memcpy(addr, ptr, chunk->size);
			free(ptr);
			ptr = addr;
		}
	}

	return ptr;
}

void *reallocarray(void *ptr, size_t nmemb, size_t size) {
#ifdef DEBUG
	fprintf(stderr, "reallocarray(%p, %d, %d)", ptr, nmemb, size);
#endif
	void *addr;

	if (size > SIZE_MAX / nmemb) return NULL;
	size *= nmemb;
	addr = realloc(ptr, size);
#ifdef DEBUG	
	fprintf(stderr, " = %p\n", addr);
#endif
	return addr;

}

size_t malloc_usable_size(void *ptr) {
	chunk_t *chunk = (chunk_t *)(ptr - sizeof(chunk_t));
	return chunk->size;
}

void *aligned_alloc(size_t alignment, size_t size) {
#ifdef DEBUG
	fprintf(stderr, "aligned_alloc(%d, %d)", alignment, size);
#endif
	
	void *addr;
	posix_memalign(&addr, alignment, size);

#ifdef DEBUG
	fprintf(stderr, " = %p\n", addr);
#endif
	return addr;
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
	void *addr;
	if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
		return EINVAL;
	} // Check whethere alignment is a power of 2 or 0
	else if (alignment > pagesize) {
		fprintf(stderr, "Alignment larger than pagesize is not supported!\n");
		exit(EXIT_FAILURE);
	}
	else {
		addr = alloc(alignment, size);
		if (errno == ENOMEM) return ENOMEM;
		*memptr = addr;
		return 0;
	}
}

