#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/** Initializes semaphore SEMA to VALUE. */
void
sema_init (struct semaphore *sema, unsigned value) 
{
  ASSERT (sema != NULL);

  sema->value = value;
  list_init (&sema->waiters);
}

/** Down or "P" operation on a semaphore. */
void
sema_down (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  while (sema->value == 0) 
    {
      /* Insert thread into waiters list in priority order. */
      list_insert_ordered (&sema->waiters, &thread_current ()->elem, thread_priority_less, NULL);
      thread_block ();
    }
  sema->value--;
  intr_set_level (old_level);
}

/** Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0. */
bool
sema_try_down (struct semaphore *sema) 
{
  enum intr_level old_level;
  bool success;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (sema->value > 0) 
    {
      sema->value--;
      success = true; 
    }
  else
    success = false;
  intr_set_level (old_level);

  return success;
}

/** Up or "V" operation on a semaphore. */
void
sema_up (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (!list_empty (&sema->waiters)) 
    {
      /* Unblock the highest-priority waiting thread. */
      thread_unblock (list_entry (list_pop_front (&sema->waiters),
                                    struct thread, elem));
    }
  sema->value++;

  /* After unblocking, check if we should yield. */
  if (!list_empty(&ready_list) &&
      thread_current()->priority < list_entry(list_front(&ready_list), struct thread, elem)->priority)
    {
      thread_yield();
    }
  
  intr_set_level (old_level);
}

static void sema_test_helper (void *sema_);

/** Self-test for semaphores. */
void
sema_self_test (void) 
{
  struct semaphore sema[2];
  int i;

  printf ("Testing semaphores...");
  sema_init (&sema[0], 0);
  sema_init (&sema[1], 0);
  thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
  for (i = 0; i < 10; i++) 
    {
      sema_up (&sema[0]);
      sema_down (&sema[1]);
    }
  printf ("done.\n");
}

/** Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) 
{
  struct semaphore *sema = sema_;
  int i;

  for (i = 0; i < 10; i++) 
    {
      sema_down (&sema[0]);
      sema_up (&sema[1]);
    }
}

/** Initializes LOCK. */
void
lock_init (struct lock *lock)
{
  ASSERT (lock != NULL);

  lock->holder = NULL;
  sema_init (&lock->semaphore, 1);
}

/** Acquires LOCK, donating priority if necessary. */
void
lock_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));

  struct thread *cur = thread_current ();

  /* If the lock is held, we may need to donate priority. */
  if (lock->holder != NULL)
    {
      cur->waiting_on_lock = lock;

      /* Donate priority iteratively up the chain of waiting threads. */
      struct thread *holder = lock->holder;
      while (holder && holder->priority < cur->priority)
        {
          holder->priority = cur->priority;
          holder = holder->waiting_on_lock ? holder->waiting_on_lock->holder : NULL;
        }
    }
    
  sema_down (&lock->semaphore);

  /* We have acquired the lock. */
  cur->waiting_on_lock = NULL;
  lock->holder = cur;
  list_push_back(&cur->locks_held, &lock->elem);
}

/** Tries to acquires LOCK and returns true if successful or false
   on failure. */
bool
lock_try_acquire (struct lock *lock)
{
  bool success;

  ASSERT (lock != NULL);
  ASSERT (!lock_held_by_current_thread (lock));

  success = sema_try_down (&lock->semaphore);
  if (success) {
    lock->holder = thread_current ();
    list_push_back(&thread_current()->locks_held, &lock->elem);
  }
  return success;
}

/** Releases LOCK, recalculating priority. */
void
lock_release (struct lock *lock) 
{
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));
  
  struct thread *cur = thread_current ();

  /* Remove lock from this thread's held list and clear holder. */
  list_remove(&lock->elem);
  lock->holder = NULL;

  /* Recalculate this thread's priority, as it may have been donated. */
  thread_recalculate_priority(cur);

  sema_up (&lock->semaphore);
}

/** Returns true if the current thread holds LOCK, false
   otherwise. */
bool
lock_held_by_current_thread (const struct lock *lock) 
{
  ASSERT (lock != NULL);

  return lock->holder == thread_current ();
}

/** One semaphore in a list. */
struct semaphore_elem 
  {
    struct list_elem elem;
    struct semaphore semaphore;
  };

/** Initializes condition variable COND. */
void
cond_init (struct condition *cond)
{
  ASSERT (cond != NULL);

  list_init (&cond->waiters);
}

/** Atomically releases LOCK and waits for COND to be signaled. */
void
cond_wait (struct condition *cond, struct lock *lock) 
{
  struct semaphore_elem waiter;

  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));
  
  sema_init (&waiter.semaphore, 0);
  list_push_back (&cond->waiters, &waiter.elem);
  lock_release (lock);
  sema_down (&waiter.semaphore);
  lock_acquire (lock);
}

/** If any threads are waiting on COND, signals one of them. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  if (!list_empty (&cond->waiters)) 
    sema_up (&list_entry (list_pop_front (&cond->waiters),
                          struct semaphore_elem, elem)->semaphore);
}

/** Wakes up all threads, if any, waiting on COND. */
void
cond_broadcast (struct condition *cond, struct lock *lock) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);

  while (!list_empty (&cond->waiters))
    cond_signal (cond, lock);
}


//Birla's commit -> made changes to synch.c and synch.h
//Now has functionality for all types of donation - single, multiple, chained everything