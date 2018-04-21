#include <switch.h>
#include <stdbool.h>

typedef __int128 unsigned I16;

/*
      memory_order_relaxed == 0
      memory_order_consume == 1
      memory_order_acquire == 2
      memory_order_release == 3
      memory_order_acq_rel == 4
      memory_order_seq_cst == 5
*/

/* The target page size.  Must be no larger than the runtime page size,
   lest locking fail with virtual address aliasing (i.e. a page mmaped
   at two locations).  */
#ifndef PAGE_SIZE
#define PAGE_SIZE	4096
#endif

/* The target cacheline size.  This is an optimization; the padding that
   should be applied to the locks to keep them from interfering.  */
#ifndef CACHLINE_SIZE
#define CACHLINE_SIZE	64
#endif

/* The granularity at which locks are applied.  Almost certainly the
   cachline size is the right thing to use here.  */
#ifndef WATCH_SIZE
#define WATCH_SIZE	CACHLINE_SIZE
#endif

struct lock
{
  Mutex mutex;
  char pad[sizeof(Mutex) < CACHLINE_SIZE
	   ? CACHLINE_SIZE - sizeof(Mutex)
	   : 0];
};

#define NLOCKS		(PAGE_SIZE / WATCH_SIZE)
static struct lock locks[NLOCKS] = {
  [0 ... NLOCKS-1].mutex = 0,
};

static inline uintptr_t 
addr_hash (volatile void *ptr)
{
  return ((uintptr_t)ptr / WATCH_SIZE) % NLOCKS;
}

void
libat_lock_1 (volatile void *ptr)
{
  mutexLock(&locks[addr_hash (ptr)].mutex);
}

void
libat_unlock_1 (volatile void *ptr)
{
  mutexUnlock(&locks[addr_hash (ptr)].mutex);
}

void
libat_lock_n (void *ptr, size_t n)
{
  uintptr_t h = addr_hash (ptr);
  size_t i = 0;

  /* Don't lock more than all the locks we have.  */
  if (n > PAGE_SIZE)
    n = PAGE_SIZE;

  do
    {
      mutexLock(&locks[h].mutex);
      if (++h == NLOCKS)
	h = 0;
      i += WATCH_SIZE;
    }
  while (i < n);
}

void
libat_unlock_n (void *ptr, size_t n)
{
  uintptr_t h = addr_hash (ptr);
  size_t i = 0;

  if (n > PAGE_SIZE)
    n = PAGE_SIZE;

  do
    {
      mutexUnlock(&locks[h].mutex);
      if (++h == NLOCKS)
	h = 0;
      i += WATCH_SIZE;
    }
  while (i < n);
}


I16 __atomic_load_16 (const volatile void *mem, int model)
{
	libat_lock_1((volatile void*)mem);	
	I16 a = *(I16*)mem;
	libat_unlock_1((volatile void*)mem);
	return a;
}

void __atomic_store_16 (volatile void *mem, I16 val, int model)
{
	libat_lock_1(mem);
	*(I16*)mem = val;
	libat_unlock_1(mem);
}

I16 __atomic_exchange_16 (volatile void *mem, I16 val, int model)
{
	libat_lock_1(mem);
	I16 a = *(I16*)mem;
	*(I16*)mem = val;
	libat_unlock_1(mem);
	return a;
}

bool __atomic_compare_exchange_16 (volatile void *mem, void *expected, I16 desired, bool weak, int success, int failure)
{
	I16 oldval;
	bool ret;
	I16 *eptr = (I16*)expected;
	I16 *mptr = (I16*)mem;

	libat_lock_1(mptr);

	oldval = *mptr;
	ret = (oldval == *eptr);
	if (ret)
		*mptr = desired;
	else
		*eptr = oldval;

	libat_unlock_1(mptr);

	return false;
}
