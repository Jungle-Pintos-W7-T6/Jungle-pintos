#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
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
#define PRI_MIN 0	   /* 최저 우선순위 */
#define PRI_DEFAULT 31 /* 기본 우선순위 */
#define PRI_MAX 63	   /* 최고 우선순위 */

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

/* 멤버 'elem'는 세 가지 용도로 사용됩니다.
 * 실행 큐(thread.c)의 요소이거나,
 * 수면 리스트(thread.c)의 요소이거나,
 * 세마포어 대기 목록(synch.c)의 요소일 수 있습니다.
 *
 * 각 상태는 상호 배타적입니다.
 * 하나의 elem은 동시에 위의 셋 중, 하나의 리스트에만 속합니다. */
struct thread
{
	/* Owned by thread.c. */
	tid_t tid;				   /* 스레드 ID */
	enum thread_status status; /* 스레드 상태 */
	char name[16];			   /* 이름 (for debugging purposes). */
	int priority;			   /* 우선순위 */
	int64_t alarm;			   /* Sleep 시 기상 시각 */

	long long recent_cpu; /* 현재 CPU 사용량 */

	/* Shared between thread.c and synch.c. */
	struct list_elem elem; /* 리스트 요소 */

	/*nuri. priority donation에 필요한 멤버들 추가*/
	int original_priority;			/* 현재 쓰레드의 원래 우선 순위 */
	struct list donations;			/* 자신에게 우선순위를 기부한 스레드들의 리스트 */
	struct list_elem donation_elem; /* 이 스레드를 다른 스레드의 donations 리스트에 연결하는 노드
									   내가 기부자로 등록될 때 사용하는 멤버*/
	struct lock *wait_on_lock;		/* 스레드가 획득하려고 기다리는 lock */

	/*nuri. [P1] "락을 넘겨받은 새 소유자에게 남은 대기자의 기부가 이어지지 않습니다.""
	  donation_target은 기부 대상을 변경할 때 기존 기부 리스트의 소유자를 찾기 위한 멤버
	  waiting_elem은 BLOCKED뿐 만 아니라 READY 상태인 LOCK 대기자까지 추적하기 위한 멤버*/

	struct thread *donation_target; /* 현재 자신이 기부자로 등록된 대상 스레드 */
	struct list_elem waiting_elem;	/*  lock의 waiting_threads 리스트에 연결할 노드 */

	int nice; /* nuri. MLFQS에서 우선순위 계산을 위해 필요한 필드. */

	struct list_elem all_elem; /* YJ: 각 스레드를 명단에 연결할 요소. 스레드를 복사하거나 새로 생성하는 작업은 없음. 기존 스레드에 연결 요소 하나를 추가하는 것. */

#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4; /* Page map level 4 */
#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf; /* 문맥 교환 정보 */
	unsigned magic;		  /* 오버플로우 감지 */
};

/* YJ: 작업 함수는 순회 중 양보하거나 목록에 스레드를 추가/제거하지 않는 제약으로 사용할 것! idle도 목록에 포함하고, 누&진스는 thread_is_idle()로 제외하면 됨.
전체 목록은 우선순위로 정렬할 필요가 없음. 모든 스레드를 빠짐없이 순회하는 목적이므로 등록할 때 뒤에 넣으면 됨. */
typedef void thread_action_func(struct thread *t, void *aux);
void thread_foreach(thread_action_func *func, void *aux);

bool thread_is_idle(const struct thread *t); // YJ
size_t thread_ready_count(void);			 // YJ
void thread_reorder_ready_queue(void);		 // YJ

/* If false (default), use round-robin scheduler.
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
void thread_sleep(int64_t alarm);
void thread_awake(int64_t ticks);

void thread_check_preemption(void);				// nuri. synch.c 등 다른 파일에서도 preemption 검사를 위해 추가
void thread_priority_changed(struct thread *t); // YJ: 우선순위 값이 바뀐 READY 스레드를 준비 큐에서 올바른 위치로 옮기는 함수

int thread_get_priority(void);
void thread_set_priority(int);
/* nuri. synch.c에서도 사용하기 위해 헤더에 priority_more 추가 */
bool priority_more(const struct list_elem *elem_prev, const struct list_elem *elem_next, void *aux UNUSED);

int thread_get_nice(void);
void thread_set_nice(int UNUSED);
void thread_update_load_avg(void);
void thread_update_recent_cpu(void);
int thread_get_recent_cpu(void);
int thread_get_load_avg(void);

void do_iret(struct intr_frame *tf);

#endif /* threads/thread.h */
