/* GLIB - Library of useful routines for C programming
 * Copyright (C) 1995-1997  Peter Mattis, Spencer Kimball and Josh MacDonald
 *
 * gthread.c: posix thread system implementation
 * Copyright 1998 Sebastian Wilhelmi; University of Karlsruhe
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Modified by the GLib Team and others 1997-2000.  See the AUTHORS
 * file for a list of people on the GLib Team.  See the ChangeLog
 * files for a list of changes.  These files are distributed with
 * GLib at ftp://ftp.gtk.org/pub/gtk/.
 */

/* The GMutex, GCond and GPrivate implementations in this file are some
 * of the lowest-level code in GLib.  All other parts of GLib (messages,
 * memory, slices, etc) assume that they can freely use these facilities
 * without risking recursion.
 *
 * As such, these functions are NOT permitted to call any other part of
 * GLib.
 *
 * The thread manipulation functions (create, exit, join, etc.) have
 * more freedom -- they can do as they please.
 */

#include "config.h"

#include "gthread.h"

#include "gthreadprivate.h"
#include "gslice.h"
#include "gmessages.h"
#include "gstrfuncs.h"
#include "gmain.h"
#include "gutils.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <sys/time.h>
#include <unistd.h>

#include <switch/result.h>
#include <switch/kernel/svc.h>
#include <switch/kernel/mutex.h>
#include <switch/kernel/condvar.h>
#include <switch/kernel/rwlock.h>
#include <switch/kernel/thread.h>

/* clang defines __ATOMIC_SEQ_CST but doesn't support the GCC extension */
#if defined(HAVE_FUTEX) && defined(__ATOMIC_SEQ_CST) && !defined(__clang__)
#define USE_NATIVE_MUTEX
#endif

static void
g_thread_abort (gint         status,
                const gchar *function)
{
  fprintf (stderr, "GLib (gthread-horizon.c): Unexpected error from C library during '%s': %s.  Aborting.\n",
           function, strerror (status));
  g_abort ();
}

/* {{{1 GMutex */

#if !defined(USE_NATIVE_MUTEX)

static Mutex *
g_mutex_impl_new (void)
{
  Mutex *mutex;
  gint status;

  mutex = malloc (sizeof (Mutex));
  if G_UNLIKELY (mutex == NULL)
    g_thread_abort (errno, "malloc");

  mutexInit(mutex);
  return mutex;
}

static void
g_mutex_impl_free (Mutex *mutex)
{
  free (mutex);
}

static inline Mutex *
g_mutex_get_impl (GMutex *mutex)
{
  Mutex *impl = g_atomic_pointer_get (&mutex->p);

  if G_UNLIKELY (impl == NULL)
    {
      impl = g_mutex_impl_new ();
      if (!g_atomic_pointer_compare_and_exchange (&mutex->p, NULL, impl))
        g_mutex_impl_free (impl);
      impl = mutex->p;
    }

  return impl;
}


/**
 * g_mutex_init:
 * @mutex: an uninitialized #GMutex
 *
 * Initializes a #GMutex so that it can be used.
 *
 * This function is useful to initialize a mutex that has been
 * allocated on the stack, or as part of a larger structure.
 * It is not necessary to initialize a mutex that has been
 * statically allocated.
 *
 * |[<!-- language="C" --> 
 *   typedef struct {
 *     GMutex m;
 *     ...
 *   } Blob;
 *
 * Blob *b;
 *
 * b = g_new (Blob, 1);
 * g_mutex_init (&b->m);
 * ]|
 *
 * To undo the effect of g_mutex_init() when a mutex is no longer
 * needed, use g_mutex_clear().
 *
 * Calling g_mutex_init() on an already initialized #GMutex leads
 * to undefined behaviour.
 *
 * Since: 2.32
 */
void
g_mutex_init (GMutex *mutex)
{
  mutex->p = g_mutex_impl_new ();
}

/**
 * g_mutex_clear:
 * @mutex: an initialized #GMutex
 *
 * Frees the resources allocated to a mutex with g_mutex_init().
 *
 * This function should not be used with a #GMutex that has been
 * statically allocated.
 *
 * Calling g_mutex_clear() on a locked mutex leads to undefined
 * behaviour.
 *
 * Sine: 2.32
 */
void
g_mutex_clear (GMutex *mutex)
{
  g_mutex_impl_free (mutex->p);
}

/**
 * g_mutex_lock:
 * @mutex: a #GMutex
 *
 * Locks @mutex. If @mutex is already locked by another thread, the
 * current thread will block until @mutex is unlocked by the other
 * thread.
 *
 * #GMutex is neither guaranteed to be recursive nor to be
 * non-recursive.  As such, calling g_mutex_lock() on a #GMutex that has
 * already been locked by the same thread results in undefined behaviour
 * (including but not limited to deadlocks).
 */
void
g_mutex_lock (GMutex *mutex)
{
  mutexLock (g_mutex_get_impl (mutex));
}

/**
 * g_mutex_unlock:
 * @mutex: a #GMutex
 *
 * Unlocks @mutex. If another thread is blocked in a g_mutex_lock()
 * call for @mutex, it will become unblocked and can lock @mutex itself.
 *
 * Calling g_mutex_unlock() on a mutex that is not locked by the
 * current thread leads to undefined behaviour.
 */
void
g_mutex_unlock (GMutex *mutex)
{
  mutexUnlock (g_mutex_get_impl (mutex));
}

/**
 * g_mutex_trylock:
 * @mutex: a #GMutex
 *
 * Tries to lock @mutex. If @mutex is already locked by another thread,
 * it immediately returns %FALSE. Otherwise it locks @mutex and returns
 * %TRUE.
 *
 * #GMutex is neither guaranteed to be recursive nor to be
 * non-recursive.  As such, calling g_mutex_lock() on a #GMutex that has
 * already been locked by the same thread results in undefined behaviour
 * (including but not limited to deadlocks or arbitrary return values).

 * Returns: %TRUE if @mutex could be locked
 */
gboolean
g_mutex_trylock (GMutex *mutex)
{
  gint status;

  if G_LIKELY ((status = mutexTryLock(g_mutex_get_impl (mutex))) == 1)
    return TRUE;

  if G_UNLIKELY (status != EBUSY)
    g_thread_abort (status, "mutexTryLock");

  return FALSE;
}

#endif /* !defined(USE_NATIVE_MUTEX) */

/* {{{1 GRecMutex */

static RMutex *
g_rec_mutex_impl_new (void)
{
  RMutex *mutex;

  mutex = malloc (sizeof (RMutex));
  if G_UNLIKELY (mutex == NULL)
    g_thread_abort (errno, "malloc");

  rmutexInit(mutex);
  return mutex;
}

static void
g_rec_mutex_impl_free (RMutex *mutex)
{
  free (mutex);
}

static inline RMutex *
g_rec_mutex_get_impl (GRecMutex *rec_mutex)
{
  RMutex *impl = g_atomic_pointer_get (&rec_mutex->p);

  if G_UNLIKELY (impl == NULL)
    {
      impl = g_rec_mutex_impl_new ();
      if (!g_atomic_pointer_compare_and_exchange (&rec_mutex->p, NULL, impl))
        g_rec_mutex_impl_free (impl);
      impl = rec_mutex->p;
    }

  return impl;
}

/**
 * g_rec_mutex_init:
 * @rec_mutex: an uninitialized #GRecMutex
 *
 * Initializes a #GRecMutex so that it can be used.
 *
 * This function is useful to initialize a recursive mutex
 * that has been allocated on the stack, or as part of a larger
 * structure.
 *
 * It is not necessary to initialise a recursive mutex that has been
 * statically allocated.
 *
 * |[<!-- language="C" --> 
 *   typedef struct {
 *     GRecMutex m;
 *     ...
 *   } Blob;
 *
 * Blob *b;
 *
 * b = g_new (Blob, 1);
 * g_rec_mutex_init (&b->m);
 * ]|
 *
 * Calling g_rec_mutex_init() on an already initialized #GRecMutex
 * leads to undefined behaviour.
 *
 * To undo the effect of g_rec_mutex_init() when a recursive mutex
 * is no longer needed, use g_rec_mutex_clear().
 *
 * Since: 2.32
 */
void
g_rec_mutex_init (GRecMutex *rec_mutex)
{
  rec_mutex->p = g_rec_mutex_impl_new ();
}

/**
 * g_rec_mutex_clear:
 * @rec_mutex: an initialized #GRecMutex
 *
 * Frees the resources allocated to a recursive mutex with
 * g_rec_mutex_init().
 *
 * This function should not be used with a #GRecMutex that has been
 * statically allocated.
 *
 * Calling g_rec_mutex_clear() on a locked recursive mutex leads
 * to undefined behaviour.
 *
 * Sine: 2.32
 */
void
g_rec_mutex_clear (GRecMutex *rec_mutex)
{
  g_rec_mutex_impl_free (rec_mutex->p);
}

/**
 * g_rec_mutex_lock:
 * @rec_mutex: a #GRecMutex
 *
 * Locks @rec_mutex. If @rec_mutex is already locked by another
 * thread, the current thread will block until @rec_mutex is
 * unlocked by the other thread. If @rec_mutex is already locked
 * by the current thread, the 'lock count' of @rec_mutex is increased.
 * The mutex will only become available again when it is unlocked
 * as many times as it has been locked.
 *
 * Since: 2.32
 */
void
g_rec_mutex_lock (GRecMutex *mutex)
{
  rmutexLock(g_rec_mutex_get_impl (mutex));
}

/**
 * g_rec_mutex_unlock:
 * @rec_mutex: a #GRecMutex
 *
 * Unlocks @rec_mutex. If another thread is blocked in a
 * g_rec_mutex_lock() call for @rec_mutex, it will become unblocked
 * and can lock @rec_mutex itself.
 *
 * Calling g_rec_mutex_unlock() on a recursive mutex that is not
 * locked by the current thread leads to undefined behaviour.
 *
 * Since: 2.32
 */
void
g_rec_mutex_unlock (GRecMutex *rec_mutex)
{
  rmutexUnlock (rec_mutex->p);
}

/**
 * g_rec_mutex_trylock:
 * @rec_mutex: a #GRecMutex
 *
 * Tries to lock @rec_mutex. If @rec_mutex is already locked
 * by another thread, it immediately returns %FALSE. Otherwise
 * it locks @rec_mutex and returns %TRUE.
 *
 * Returns: %TRUE if @rec_mutex could be locked
 *
 * Since: 2.32
 */
gboolean
g_rec_mutex_trylock (GRecMutex *rec_mutex)
{
  if (rmutexTryLock (g_rec_mutex_get_impl (rec_mutex)) != 0)
    return FALSE;

  return TRUE;
}

/* {{{1 GRWLock */

static RwLock *
g_rw_lock_impl_new (void)
{
  RwLock *rwlock;
  gint status;

  rwlock = malloc (sizeof (RwLock));
  if G_UNLIKELY (rwlock == NULL)
    g_thread_abort (errno, "malloc");

  rmutexInit(&rwlock->r);
  rmutexInit(&rwlock->g);

  return rwlock;
}

static void
g_rw_lock_impl_free (RwLock *rwlock)
{
  free (rwlock);
}

static inline RwLock *
g_rw_lock_get_impl (GRWLock *lock)
{
  RwLock *impl = g_atomic_pointer_get (&lock->p);

  if G_UNLIKELY (impl == NULL)
    {
      impl = g_rw_lock_impl_new ();
      if (!g_atomic_pointer_compare_and_exchange (&lock->p, NULL, impl))
        g_rw_lock_impl_free (impl);
      impl = lock->p;
    }

  return impl;
}

/**
 * g_rw_lock_init:
 * @rw_lock: an uninitialized #GRWLock
 *
 * Initializes a #GRWLock so that it can be used.
 *
 * This function is useful to initialize a lock that has been
 * allocated on the stack, or as part of a larger structure.  It is not
 * necessary to initialise a reader-writer lock that has been statically
 * allocated.
 *
 * |[<!-- language="C" --> 
 *   typedef struct {
 *     GRWLock l;
 *     ...
 *   } Blob;
 *
 * Blob *b;
 *
 * b = g_new (Blob, 1);
 * g_rw_lock_init (&b->l);
 * ]|
 *
 * To undo the effect of g_rw_lock_init() when a lock is no longer
 * needed, use g_rw_lock_clear().
 *
 * Calling g_rw_lock_init() on an already initialized #GRWLock leads
 * to undefined behaviour.
 *
 * Since: 2.32
 */
void
g_rw_lock_init (GRWLock *rw_lock)
{
  rw_lock->p = g_rw_lock_impl_new ();
}

/**
 * g_rw_lock_clear:
 * @rw_lock: an initialized #GRWLock
 *
 * Frees the resources allocated to a lock with g_rw_lock_init().
 *
 * This function should not be used with a #GRWLock that has been
 * statically allocated.
 *
 * Calling g_rw_lock_clear() when any thread holds the lock
 * leads to undefined behaviour.
 *
 * Sine: 2.32
 */
void
g_rw_lock_clear (GRWLock *rw_lock)
{
  g_rw_lock_impl_free (rw_lock->p);
}

/**
 * g_rw_lock_writer_lock:
 * @rw_lock: a #GRWLock
 *
 * Obtain a write lock on @rw_lock. If any thread already holds
 * a read or write lock on @rw_lock, the current thread will block
 * until all other threads have dropped their locks on @rw_lock.
 *
 * Since: 2.32
 */
void
g_rw_lock_writer_lock (GRWLock *rw_lock)
{
  rwlockWriteLock (g_rw_lock_get_impl (rw_lock));
}

/**
 * g_rw_lock_writer_trylock:
 * @rw_lock: a #GRWLock
 *
 * Tries to obtain a write lock on @rw_lock. If any other thread holds
 * a read or write lock on @rw_lock, it immediately returns %FALSE.
 * Otherwise it locks @rw_lock and returns %TRUE.
 *
 * Returns: %TRUE if @rw_lock could be locked
 *
 * Since: 2.32
 */
gboolean
g_rw_lock_writer_trylock (GRWLock *rw_lock)
{
  RwLock *rw = g_rw_lock_get_impl(rw_lock);
  if(rmutexTryLock(&rw->g))
  {
    return TRUE;
  }
  return FALSE;
}

/**
 * g_rw_lock_writer_unlock:
 * @rw_lock: a #GRWLock
 *
 * Release a write lock on @rw_lock.
 *
 * Calling g_rw_lock_writer_unlock() on a lock that is not held
 * by the current thread leads to undefined behaviour.
 *
 * Since: 2.32
 */
void
g_rw_lock_writer_unlock (GRWLock *rw_lock)
{
  rwlockWriteUnlock(g_rw_lock_get_impl (rw_lock));
}

/**
 * g_rw_lock_reader_lock:
 * @rw_lock: a #GRWLock
 *
 * Obtain a read lock on @rw_lock. If another thread currently holds
 * the write lock on @rw_lock or blocks waiting for it, the current
 * thread will block. Read locks can be taken recursively.
 *
 * It is implementation-defined how many threads are allowed to
 * hold read locks on the same lock simultaneously.
 *
 * Since: 2.32
 */
void
g_rw_lock_reader_lock (GRWLock *rw_lock)
{
  rwlockReadLock (g_rw_lock_get_impl (rw_lock));
}

/**
 * g_rw_lock_reader_trylock:
 * @rw_lock: a #GRWLock
 *
 * Tries to obtain a read lock on @rw_lock and returns %TRUE if
 * the read lock was successfully obtained. Otherwise it
 * returns %FALSE.
 *
 * Returns: %TRUE if @rw_lock could be locked
 *
 * Since: 2.32
 */
gboolean
g_rw_lock_reader_trylock (GRWLock *rw_lock)
{
  RwLock *rw = g_rw_lock_get_impl(rw_lock);
  if(!rmutexTryLock(&rw->r)) return FALSE;

  if (rw->b++ == 0)
  {
    if(!rmutexTryLock(&rw->g))
    {
      rw->b--;
      return FALSE;
    }
  }

  rmutexUnlock(&rw->r);

  return TRUE;
}

/**
 * g_rw_lock_reader_unlock:
 * @rw_lock: a #GRWLock
 *
 * Release a read lock on @rw_lock.
 *
 * Calling g_rw_lock_reader_unlock() on a lock that is not held
 * by the current thread leads to undefined behaviour.
 *
 * Since: 2.32
 */
void
g_rw_lock_reader_unlock (GRWLock *rw_lock)
{
  rwlockReadUnlock (g_rw_lock_get_impl (rw_lock));
}

/* {{{1 GCond */

#if !defined(USE_NATIVE_MUTEX)

static CondVar *
g_cond_impl_new (void)
{
  CondVar *cond;
  gint status;

  Mutex *m = malloc(sizeof(Mutex));
  if G_UNLIKELY(m == NULL)
    g_thread_abort (errno, "malloc");

  cond = malloc (sizeof (CondVar));
  if G_UNLIKELY (cond == NULL)
    g_thread_abort (errno, "malloc");

  condvarInit(cond, m);
  return cond;
}

static void
g_cond_impl_free (CondVar *cond)
{
  free(cond->mutex);
  free (cond);
}

static inline CondVar *
g_cond_get_impl (GCond *cond)
{
  CondVar *impl = g_atomic_pointer_get (&cond->p);

  if G_UNLIKELY (impl == NULL)
    {
      impl = g_cond_impl_new ();
      if (!g_atomic_pointer_compare_and_exchange (&cond->p, NULL, impl))
        g_cond_impl_free (impl);
      impl = cond->p;
    }

  return impl;
}

/**
 * g_cond_init:
 * @cond: an uninitialized #GCond
 *
 * Initialises a #GCond so that it can be used.
 *
 * This function is useful to initialise a #GCond that has been
 * allocated as part of a larger structure.  It is not necessary to
 * initialise a #GCond that has been statically allocated.
 *
 * To undo the effect of g_cond_init() when a #GCond is no longer
 * needed, use g_cond_clear().
 *
 * Calling g_cond_init() on an already-initialised #GCond leads
 * to undefined behaviour.
 *
 * Since: 2.32
 */
void
g_cond_init (GCond *cond)
{
  cond->p = g_cond_impl_new ();
}

/**
 * g_cond_clear:
 * @cond: an initialised #GCond
 *
 * Frees the resources allocated to a #GCond with g_cond_init().
 *
 * This function should not be used with a #GCond that has been
 * statically allocated.
 *
 * Calling g_cond_clear() for a #GCond on which threads are
 * blocking leads to undefined behaviour.
 *
 * Since: 2.32
 */
void
g_cond_clear (GCond *cond)
{
  g_cond_impl_free (cond->p);
}

/**
 * g_cond_wait:
 * @cond: a #GCond
 * @mutex: a #GMutex that is currently locked
 *
 * Atomically releases @mutex and waits until @cond is signalled.
 * When this function returns, @mutex is locked again and owned by the
 * calling thread.
 *
 * When using condition variables, it is possible that a spurious wakeup
 * may occur (ie: g_cond_wait() returns even though g_cond_signal() was
 * not called).  It's also possible that a stolen wakeup may occur.
 * This is when g_cond_signal() is called, but another thread acquires
 * @mutex before this thread and modifies the state of the program in
 * such a way that when g_cond_wait() is able to return, the expected
 * condition is no longer met.
 *
 * For this reason, g_cond_wait() must always be used in a loop.  See
 * the documentation for #GCond for a complete example.
 **/
void
g_cond_wait (GCond  *cond,
             GMutex *mutex)
{
  gint status;
  // TDOO: this is terrible
  CondVar *real_cond = g_cond_get_impl (cond);
  real_cond->mutex = g_mutex_get_impl(mutex);
  if G_UNLIKELY ((status =  condvarWait(real_cond)) != 0)
    g_thread_abort (status, "condvarWait");
}

/**
 * g_cond_signal:
 * @cond: a #GCond
 *
 * If threads are waiting for @cond, at least one of them is unblocked.
 * If no threads are waiting for @cond, this function has no effect.
 * It is good practice to hold the same lock as the waiting thread
 * while calling this function, though not required.
 */
void
g_cond_signal (GCond *cond)
{
  gint status;

  if G_UNLIKELY ((status = condvarWakeOne (g_cond_get_impl (cond))) != 0)
    g_thread_abort (status, "condvarWakeOne");
}

/**
 * g_cond_broadcast:
 * @cond: a #GCond
 *
 * If threads are waiting for @cond, all of them are unblocked.
 * If no threads are waiting for @cond, this function has no effect.
 * It is good practice to lock the same mutex as the waiting threads
 * while calling this function, though not required.
 */
void
g_cond_broadcast (GCond *cond)
{
  gint status;

  if G_UNLIKELY ((status = condvarWakeAll (g_cond_get_impl (cond))) != 0)
    g_thread_abort (status, "condvarWakeAll");
}

/**
 * g_cond_wait_until:
 * @cond: a #GCond
 * @mutex: a #GMutex that is currently locked
 * @end_time: the monotonic time to wait until
 *
 * Waits until either @cond is signalled or @end_time has passed.
 *
 * As with g_cond_wait() it is possible that a spurious or stolen wakeup
 * could occur.  For that reason, waiting on a condition variable should
 * always be in a loop, based on an explicitly-checked predicate.
 *
 * %TRUE is returned if the condition variable was signalled (or in the
 * case of a spurious wakeup).  %FALSE is returned if @end_time has
 * passed.
 *
 * The following code shows how to correctly perform a timed wait on a
 * condition variable (extending the example presented in the
 * documentation for #GCond):
 *
 * |[<!-- language="C" --> 
 * gpointer
 * pop_data_timed (void)
 * {
 *   gint64 end_time;
 *   gpointer data;
 *
 *   g_mutex_lock (&data_mutex);
 *
 *   end_time = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
 *   while (!current_data)
 *     if (!g_cond_wait_until (&data_cond, &data_mutex, end_time))
 *       {
 *         // timeout has passed.
 *         g_mutex_unlock (&data_mutex);
 *         return NULL;
 *       }
 *
 *   // there is data for us
 *   data = current_data;
 *   current_data = NULL;
 *
 *   g_mutex_unlock (&data_mutex);
 *
 *   return data;
 * }
 * ]|
 *
 * Notice that the end time is calculated once, before entering the
 * loop and reused.  This is the motivation behind the use of absolute
 * time on this API -- if a relative time of 5 seconds were passed
 * directly to the call and a spurious wakeup occurred, the program would
 * have to start over waiting again (which would lead to a total wait
 * time of more than 5 seconds).
 *
 * Returns: %TRUE on a signal, %FALSE on a timeout
 * Since: 2.32
 **/
gboolean
g_cond_wait_until (GCond  *cond,
                   GMutex *mutex,
                   gint64  end_time)
{
  // TODO: can i just not use the mutex?s

  gint status;
  Result res;

  /* end_time is given relative to the monotonic clock as returned by
   * g_get_monotonic_time().
   *
   * Since horizon wants the relative time, convert it back again.
   */

  gint64 now = g_get_monotonic_time ();
  gint64 relative;

  if (end_time <= now)
    return FALSE;

  relative = end_time - now;
  res = condvarWaitTimeout(g_cond_get_impl (cond), relative);

  if(res == 0xea01)
    return FALSE;
  return TRUE;
}

#endif /* defined(USE_NATIVE_MUTEX) */

/* {{{1 GPrivate */

/**
 * GPrivate:
 *
 * The #GPrivate struct is an opaque data structure to represent a
 * thread-local data key. It is approximately equivalent to the
 * pthread_setspecific()/pthread_getspecific() APIs on POSIX and to
 * TlsSetValue()/TlsGetValue() on Windows.
 *
 * If you don't already know why you might want this functionality,
 * then you probably don't need it.
 *
 * #GPrivate is a very limited resource (as far as 128 per program,
 * shared between all libraries). It is also not possible to destroy a
 * #GPrivate after it has been used. As such, it is only ever acceptable
 * to use #GPrivate in static scope, and even then sparingly so.
 *
 * See G_PRIVATE_INIT() for a couple of examples.
 *
 * The #GPrivate structure should be considered opaque.  It should only
 * be accessed via the g_private_ functions.
 */

/**
 * G_PRIVATE_INIT:
 * @notify: a #GDestroyNotify
 *
 * A macro to assist with the static initialisation of a #GPrivate.
 *
 * This macro is useful for the case that a #GDestroyNotify function
 * should be associated the key.  This is needed when the key will be
 * used to point at memory that should be deallocated when the thread
 * exits.
 *
 * Additionally, the #GDestroyNotify will also be called on the previous
 * value stored in the key when g_private_replace() is used.
 *
 * If no #GDestroyNotify is needed, then use of this macro is not
 * required -- if the #GPrivate is declared in static scope then it will
 * be properly initialised by default (ie: to all zeros).  See the
 * examples below.
 *
 * |[<!-- language="C" --> 
 * static GPrivate name_key = G_PRIVATE_INIT (g_free);
 *
 * // return value should not be freed
 * const gchar *
 * get_local_name (void)
 * {
 *   return g_private_get (&name_key);
 * }
 *
 * void
 * set_local_name (const gchar *name)
 * {
 *   g_private_replace (&name_key, g_strdup (name));
 * }
 *
 *
 * static GPrivate count_key;   // no free function
 *
 * gint
 * get_local_count (void)
 * {
 *   return GPOINTER_TO_INT (g_private_get (&count_key));
 * }
 *
 * void
 * set_local_count (gint count)
 * {
 *   g_private_set (&count_key, GINT_TO_POINTER (count));
 * }
 * ]|
 *
 * Since: 2.32
 **/


//#error GPrivate!

static void *
g_private_impl_new (GDestroyNotify notify)
{
  svcOutputDebugString("g_private_impl_new", strlen("g_private_impl_new"));
  svcBreak(0,0,0);
  return NULL;
}

static void
g_private_impl_free (void *key)
{
  svcOutputDebugString("g_private_impl_free", strlen("g_private_impl_free"));
  svcBreak(0,0,0);
}

static inline void *
g_private_get_impl (GPrivate *key)
{
  void *impl = g_atomic_pointer_get (&key->p);

  if G_UNLIKELY (impl == NULL)
    {
      impl = g_private_impl_new (key->notify);
      if (!g_atomic_pointer_compare_and_exchange (&key->p, NULL, impl))
        {
          g_private_impl_free (impl);
          impl = key->p;
        }
    }

  return impl;
}

/**
 * g_private_get:
 * @key: a #GPrivate
 *
 * Returns the current value of the thread local variable @key.
 *
 * If the value has not yet been set in this thread, %NULL is returned.
 * Values are never copied between threads (when a new thread is
 * created, for example).
 *
 * Returns: the thread-local value
 */
gpointer
g_private_get (GPrivate *key)
{
  svcOutputDebugString("g_private_get", strlen("g_private_get"));
  svcBreak(0,0,0);
  return NULL;
  /* quote POSIX: No errors are returned from pthread_getspecific(). */
  //return pthread_getspecific (*g_private_get_impl (key));
}

/**
 * g_private_set:
 * @key: a #GPrivate
 * @value: the new value
 *
 * Sets the thread local variable @key to have the value @value in the
 * current thread.
 *
 * This function differs from g_private_replace() in the following way:
 * the #GDestroyNotify for @key is not called on the old value.
 */
void
g_private_set (GPrivate *key,
               gpointer  value)
{
  svcOutputDebugString("g_private_set", strlen("g_private_set"));
  svcBreak(0,0,0);
  //gint status;

  //if G_UNLIKELY ((status = pthread_setspecific (*g_private_get_impl (key), value)) != 0)
  //  g_thread_abort (status, "pthread_setspecific");
}

/**
 * g_private_replace:
 * @key: a #GPrivate
 * @value: the new value
 *
 * Sets the thread local variable @key to have the value @value in the
 * current thread.
 *
 * This function differs from g_private_set() in the following way: if
 * the previous value was non-%NULL then the #GDestroyNotify handler for
 * @key is run on it.
 *
 * Since: 2.32
 **/
void
g_private_replace (GPrivate *key,
                   gpointer  value)
{
  svcOutputDebugString("g_private_replace", strlen("g_private_replace"));
  svcBreak(0,0,0);
  /*pthread_key_t *impl = g_private_get_impl (key);
  gpointer old;
  gint status;

  old = pthread_getspecific (*impl);
  if (old && key->notify)
    key->notify (old);

  if G_UNLIKELY ((status = pthread_setspecific (*impl, value)) != 0)
    g_thread_abort (status, "pthread_setspecific");*/
}

/* {{{1 GThread */

typedef struct
{
  GRealThread thread;

  Thread system_thread;
  gboolean  joined;
  GMutex    lock;
} GThreadHorizon;

void
g_system_thread_free (GRealThread *thread)
{
  GThreadHorizon *pt = (GThreadHorizon *) thread;

  if (!pt->joined)
    threadClose(&pt->system_thread);

  g_mutex_clear (&pt->lock);

  g_slice_free (GThreadHorizon, pt);
}

GRealThread *
g_system_thread_new (GThreadFunc   thread_func,
                     gulong        stack_size,
                     GError      **error)
{
  GThreadHorizon *thread;
  Result ret;

  thread = g_slice_new0 (GThreadHorizon);

  //  ret = pthread_create (&thread->system_thread, &attr, (void* (*)(void*))thread_func, thread);

  ret = threadCreate(&thread->system_thread, (void (*)(void*))thread_func, thread, stack_size, 0x2c, -2);

  if (R_FAILED(ret))
  {
      g_set_error (error, G_THREAD_ERROR, G_THREAD_ERROR_AGAIN, 
                   "Error creating thread: %s", g_strerror (ret));
      g_slice_free (GThreadHorizon, thread);
      return NULL;
    }

  ret = threadStart(&thread->system_thread);

  g_mutex_init (&thread->lock);

  return (GRealThread *) thread;
}

/**
 * g_thread_yield:
 *
 * Causes the calling thread to voluntarily relinquish the CPU, so
 * that other threads can run.
 *
 * This function is often used as a method to make busy wait less evil.
 */
void
g_thread_yield (void)
{
  svcSleepThread(0);
}

void
g_system_thread_wait (GRealThread *thread)
{
  GThreadHorizon *pt = (GThreadHorizon *) thread;

  g_mutex_lock (&pt->lock);

  if (!pt->joined)
    {
      threadWaitForExit(&pt->system_thread);
      pt->joined = TRUE;
    }

  g_mutex_unlock (&pt->lock);
}

void
g_system_thread_exit (void)
{
  svcExitThread();
}

void
g_system_thread_set_name (const gchar *name)
{
}

  /* {{{1 Epilogue */
/* vim:set foldmethod=marker: */