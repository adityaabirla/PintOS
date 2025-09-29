#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>

/* Forward declaration of struct lock. */
struct lock;

/** States in a thread's life cycle. */
enum thread_status {
  THREAD_RUNNING, /**< Running thread. */
  THREAD_READY,   /**< Not running but ready to run. */
  THREAD_BLOCKED, /**< Waiting for an event to trigger. */
  THREAD_DYING    /**< About to be destroyed. */
};

/** Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) - 1) /**< Error value for tid_t. */

/** Thread priorities. */
#define PRI_MIN 0      /**< Lowest priority. */
#define PRI_DEFAULT 31 /**< Default priority. */
#define PRI_MAX 63     /**< Highest priority. */

/** A kernel thread or user process. */
struct thread {
  /* Owned by thread.c. */
  tid_t tid;                 /**< Thread identifier. */
  enum thread_status status; /**< Thread state. */
  char name[16];             /**< Name (for debugging purposes). */
  uint8_t *stack;            /**< Saved stack pointer. */

  /* MODIFIED FOR PRIORITY DONATION */
  int priority;                 /**< Effective priority (may be donated). */
  int base_priority;            /**< Original priority, without donation. */
  struct lock *waiting_on_lock; /**< The lock this thread is waiting for. */
  struct list locks_held;       /**< List of locks this thread holds. */

  struct list_elem allelem; /**< List element for all threads list. */

  /* Shared between thread.c and synch.c. */
  struct list_elem elem; /**< List element. */
  struct list *fd_list;
  int next_fd; /**< Next file descriptor. */

#ifdef USERPROG
  /* Owned by userprog/process.c. */
  uint32_t *pagedir; /**< Page directory. */
#endif

  /* Owned by thread.c. */
  unsigned magic; /**< Detects stack overflow. */
};

/* Declaration of the ready list to make it globally accessible. */
extern struct list ready_list;

/** If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init(void);
void thread_start(void);

void thread_tick(void);
void thread_print_stats(void);

typedef void thread_func(void *aux);
tid_t thread_create(const char *name, int priority, thread_func *, void *);

void thread_block(void);
void thread_unblock(struct thread *);

struct thread *thread_current(void);
tid_t thread_tid(void);
const char *thread_name(void);

void thread_exit(void) NO_RETURN;
void thread_yield(void);

/** Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func(struct thread *t, void *aux);
void thread_foreach(thread_action_func *, void *);

int thread_get_priority(void);
void thread_set_priority(int);

int thread_get_nice(void);
void thread_set_nice(int);
int thread_get_recent_cpu(void);
int thread_get_load_avg(void);

/* Comparison function for threads based on priority. */
bool thread_priority_less(const struct list_elem *a, const struct list_elem *b,
                          void *aux);

/* ADDED: Recalculates a thread's effective priority after a donation change. */
void thread_recalculate_priority(struct thread *t);

#endif /**< threads/thread.h */