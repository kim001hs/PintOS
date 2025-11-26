#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#include "threads/synch.h"
#ifdef VM
#include "vm/vm.h"
#endif

/* States in a thread's life cycle. */
enum thread_status
{
	THREAD_RUNNING, /* Running thread. */
	THREAD_READY,	/* Not running but ready to run. */
	THREAD_BLOCKED, /* Waiting for an event to trigger. */
	THREAD_DYING	/* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) - 1) /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0	   /* Lowest priority. */
#define PRI_DEFAULT 31 /* Default priority. */
#define PRI_MAX 63	   /* Highest priority. */

#define STDIN (struct file *)1
#define STDOUT (struct file *)2
/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * The upshot of this is twofold:
 *
 *    1. First, `struct thread' must not be allowed to grow too
 *       big.  If it does, then there will not be enough room for
 *       the kernel stack.  Our base `struct thread' is only a
 *       few bytes in size.  It probably should stay well under 1
 *       kB.
 *
 *    2. Second, kernel stacks must not be allowed to grow too
 *       large.  If a stack overflows, it will corrupt the thread
 *       state.  Thus, kernel functions should not allocate large
 *       structures or arrays as non-static local variables.  Use
 *       dynamic allocation with malloc() or palloc_get_page()
 *       instead.
 *
 * The first symptom of either of these problems will probably be
 * an assertion failure in thread_current(), which checks that
 * the `magic' member of the running thread's `struct thread' is
 * set to THREAD_MAGIC.  Stack overflow will normally change this
 * value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
 * the run queue (thread.c), or it can be an element in a
 * semaphore wait list (synch.c).  It can be used these two ways
 * only because they are mutually exclusive: only a thread in the
 * ready state is on the run queue, whereas only a thread in the
 * blocked state is on a semaphore wait list. */

/*
 * @param tid_t tid Thread identifier.
 * @param enum thread_status status;	//THREAD_RUNNING, THREAD_READY, THREAD_BLOCKED, THREAD_DYING
 * @param char name[16];
 * @param struct list_elem elem  	//sleeping_list, ready_list, waiter_list에 속할 수 있음
 * @param int priority;				//현재 우선순위
 * @param int original_priority  	// 이 스레드의 기존 우선순위(기부받기 전)
 * @param int64_t wakeup_tick	 	// 일어날 시간
 * @param struct list locks_hold 	// 스레드가 가지고 있는 락의 리스트 정렬없음
 */
struct thread
{
	/* Owned by thread.c. */
	tid_t tid;				   /* 스레드 식별자(Thread ID) */
	enum thread_status status; /* 스레드 상태(RUNNING, READY, BLOCKED 등) */
	char name[16];			   /* 스레드 이름(디버깅 용도) */
	int priority;			   /* 현재 스레드 우선순위 */

	/* thread.c와 synch.c에서 공유되는 요소 */
	struct list_elem elem;	   /* 리스트 요소. sleeping_list, ready_list, waiter_list 등 여러 리스트에 속할 수 있음 */
	struct list_elem all_elem; /* 모든 스레드가 포함된 all_list의 리스트 요소 */
	/* 새로 추가된 필드 */
	int64_t wakeup_tick;	   /* 스레드가 깨어날 시점(틱 단위) */
	int original_priority;	   /* 우선순위 기부 전 원래 스레드 우선순위 */
	struct list locks_hold;	   /* 스레드가 보유한 락들의 리스트(순서 없음) */
	struct lock *waiting_lock; /* 현재 스레드가 기다리고 있는 락 */
	int nice;				   /* 나이스 값(스케줄링에서 사용) */
	int recent_cpu;			   /* 최근 CPU 사용량(스케줄링 계산용) */
#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4; /* Page map level 4 */
	// userprog
	int exit_status;
	struct file **fd_table;
	int fd_table_size;
	struct semaphore fork_sema;
	struct semaphore wait_sema;
	struct semaphore exit_sema;
	bool waited;
	struct list child_list;
	struct list_elem child_elem;
	struct file *running_file; /* 이 스레드가 실행 중인 실행 파일(executable)을 가리키는 포인터 (load()시 저장) */
#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf; /* Information for switching */
	unsigned magic;		  /* Detects stack overflow. */
};

/* 	If false (default), use round-robin scheduler.
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

int thread_get_priority(void);
void thread_set_priority(int);

int thread_get_nice(void);
void thread_set_nice(int);
int thread_get_recent_cpu(void);
int thread_get_load_avg(void);

void do_iret(struct intr_frame *tf);

// 추가한 함수들
void preempt_priority(void);
bool priority_greater(const struct list_elem *a_, const struct list_elem *b_, void *aux UNUSED);
void sort_readylist(void);
void update_load_avg(void);
void cal_priority(struct thread *t);
void update_recent_cpu_all(void);
void update_priority_all(void);
void mlfqs_on_tick(void);
#endif /* threads/thread.h */
