#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* Sleep에 들어간 스레드의 리스트.
   해당 리스트 안의 스레드들은 THREAD_BLOCKED 상태를 가진다.*/
static struct list sleep_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */

static long long load_avg; /* 가중치 평균 */

#define F (1 << 14)

/* Scheduling. */
#define TIME_SLICE 4		  /* # of timer ticks to give each thread. */
static unsigned thread_ticks; /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
If true, use multi-level feedback queue scheduler.
Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread(thread_func *, void *aux);

static void idle(void *idle_started_ UNUSED);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule(void);
static tid_t allocate_tid(void);

void thread_update_load_avg(void);

/* YJ: 더 높은 우선순위 스레드가 준비됐으면 CPU 양보.
   인터럽트 안에서는 intr_yield_on_return() 사용. */
void thread_check_preemption(void);

/* YJ: 우선순위 값이 바뀐 READY 스레드를 준비 큐에서 올바른 위치로 옮기는 함수 */
void thread_priority_changed(struct thread *t);

/* nuri. synch.c에서도 include해서 사용하기 위해 static 상태 삭제 */
bool priority_more(const struct list_elem *elem_prev, const struct list_elem *elem_next, void *aux);

/* 기상 시각 비교를 위한 내부 헬퍼 함수 */
static bool compare_alarm(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);

/* nuri_MLFQS. 우선순위 재계산 함수 */
void mlfqs_update_priority(struct thread *t);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *)(pg_round_down(rrsp())))

// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = {0, 0x00af9a000000ffff, 0x00cf92000000ffff};

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void thread_init(void)
{
	ASSERT(intr_get_level() == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof(gdt) - 1,
		.address = (uint64_t)gdt};
	lgdt(&gdt_ds);

	/* Init the globla thread context */
	lock_init(&tid_lock);
	list_init(&ready_list);
	list_init(&sleep_list);
	list_init(&destruction_req);

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread();
	init_thread(initial_thread, "main", PRI_DEFAULT);

	/* nuri_MLFQS. MLFQS 모드에서는 최초 스레드의 nice와 recent_cpu가 0이므로
	   우선순위를 PRI_MAX로 초기화 why? 계산식 대입해보면 PRI_MAX가 됨. */
	if (thread_mlfqs)
	{
		initial_thread->priority = PRI_MAX;
	}

	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void thread_start(void)
{
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init(&idle_started, 0);
	thread_create("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down(&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void thread_tick(void)
{
	struct thread *t = thread_current();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return();
}

/* Prints thread statistics. */
void thread_print_stats(void)
{
	printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
		   idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t thread_create(const char *name, int priority,
					thread_func *function, void *aux)
{
	struct thread *t;
	tid_t tid;

	ASSERT(function != NULL);

	/* Allocate thread. */
	t = palloc_get_page(PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread(t, name, priority);

	/* nuri_MLFQS. 부모 스레드의 nice 값을 새로 생성한 t의 nice에 복사
	   Q. 부모 스레드? 자식 스레드?
	   A. 스레드를 생성한 쪽과 생성된 쪽을 구분하려는 임의 정의
	   기존 priority scheduling에서도 이 관계는 있었으나, 우선순위를 결정하는 방식이
	   priority 인자를 받는 방식이어서 상속을 고려할 필요가 없었던 것. Pintos에서는
	   스레드가 다른 스레드를 생성할 수 있는데, 하나의 스레드가 모든 작업을 순차적으로
	   처리하지 않고 여러 작업을 나누어 수행하기 위해서. 그러기 위한 thread_create() 함수 */
	if (thread_mlfqs == true)
	{
		t->nice = thread_current()->nice;
		t->recent_cpu = thread_current()->recent_cpu;

		// mlfqs_update_priority(t);
	}
	tid = t->tid = allocate_tid();

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t)kernel_thread;
	t->tf.R.rdi = (uint64_t)function;
	t->tf.R.rsi = (uint64_t)aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. */
	thread_unblock(t);

	/*	1. 새 스레드 초기화
			우선순위, 실행할 함수, 레지스터 등을 설정해서 실행 가능한 상태로 만든다.
		2. thread_unblock(t)
			새 스레드를 준비 큐에 넣고 READY로 바꾼다.
		3. thread_check_preemption() ← 지금 위치
			준비 큐의 최고 우선순위가 현재 스레드보다 높으면 CPU를 양보한다.
		4. return tid
			현재 스레드가 다시 실행되면 생성한 스레드의 ID를 반환한다.
		thread_unblock() 앞에 두면 새 스레드가 아직 준비 큐에 없어서 비교 대상에 포함되지 않음.
		따라서 준비 큐에 넣은 직후, 생성 함수가 반환하기 전에 검사.
		예를 들어 현재 우선순위가 31, 새 스레드가 40이면, 새 스레드가 thread_create()의 반환보다 먼저 실행될 수 있음. */
	thread_check_preemption(); // YJ: 더 높은 우선순위 스레드가 준비(생성)됐으면 CPU 양보. 인터럽트 안에서는 intr_yield_on_return() 사용.

	return tid;
}

/* 현재 스레드를 BLOCKED로 바꾸고 schedule()을 호출한다.

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void thread_block(void)
{
	ASSERT(!intr_context());
	ASSERT(intr_get_level() == INTR_OFF);
	/* 현재 스레드의 상태를 BLOCKED로 설정한다. */
	thread_current()->status = THREAD_BLOCKED;
	/* 이어서 기다리고 있는 스레드들을 스케줄링 한다. */
	schedule();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void thread_unblock(struct thread *t)
{
	enum intr_level old_level;

	ASSERT(is_thread(t));

	old_level = intr_disable();
	ASSERT(t->status == THREAD_BLOCKED);

	/* priority를 고려하여 READY Queue에 삽입 */
	list_insert_ordered(&ready_list, &t->elem, priority_more, NULL);
	t->status = THREAD_READY;

	intr_set_level(old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name(void)
{
	return thread_current()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current(void)
{
	struct thread *t = running_thread();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT(is_thread(t));
	ASSERT(t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t thread_tid(void)
{
	return thread_current()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void thread_exit(void)
{
	ASSERT(!intr_context());

#ifdef USERPROG
	process_exit();
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable();
	do_schedule(THREAD_DYING);
	NOT_REACHED();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void thread_yield(void)
{
	struct thread *curr = thread_current();
	enum intr_level old_level;

	ASSERT(!intr_context());

	old_level = intr_disable();

	if (curr != idle_thread)
		list_insert_ordered(&ready_list, &curr->elem, priority_more, NULL);
	do_schedule(THREAD_READY);

	intr_set_level(old_level);
}
/* 현재 RUNNING중인 스레드를 sleep 시킵니다.
 * 해당 스레드는 BLOCKED 상태로 변경되며, sleep_list로 들어갑니다. */
void thread_sleep(int64_t alarm)
{
	struct thread *curr = thread_current();
	enum intr_level old_level;

	old_level = intr_disable();

	curr->alarm = alarm;
	/* SLEEP 리스트에 시간 우선순위 대로 삽입 */
	list_insert_ordered(&sleep_list, &(curr->elem), compare_alarm, NULL);
	/* BLOCKED 상태로 변경 */
	thread_block();

	intr_set_level(old_level);
}

/* sleep_list에 들어갈 순서를 비교합니다.
 *
 *	list_insert_ordered 인자로 사용됩니다.
 *  sleep_list에는 기상 시각이 빠른 순서로 들어가야 하며,
 * 해당 비교 함수는 첫 번째 인자의 기상 시각이 더 빠를 시 1,
 * 두 번째 인자가 더 빠를 시 0을 반환합니다. */
static bool
compare_alarm(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
	struct thread *a_entry = list_entry(a, struct thread, elem);
	struct thread *b_entry = list_entry(b, struct thread, elem);

	return a_entry->alarm < b_entry->alarm;
}

/* nuri_MLFQS. 우선순위 재계산함수
   로직:
   1. t가 idle 스레드인지 확인합니다.
   2. idle 스레드라면 계산하지 않고 반환합니다.
   3. 일반 스레드라면 MLFQS 공식으로 우선순위를 계산합니다.
   4. 결과를 PRI_MIN~PRI_MAX 범위로 제한합니다.
   5. t->priority에 저장합니다. */
void mlfqs_update_priority(struct thread *t)
{
	if (t == idle_thread)
	{
		return;
	}

	int new_priority; /* nuri_MLFQS. MLFQS 공식으로 계산한 새로운 priority 값 계산 함수 필요 */

	// new_priority = ; // <- 여기에 계산한 값 저장.

	/* 계산한 값 범위 제한 */
	if (new_priority < PRI_MIN)
	{
		new_priority = PRI_MIN;
	}
	if (new_priority > PRI_MAX)
	{
		new_priority = PRI_MAX;
	}

	t->priority = new_priority;
}

/* 시간이 된 스레드를 깨워서 READY 큐로 넣습니다.
   sleep_list에서 먼저 제거된뒤, ready_list로 들어갑니다. */
void thread_awake(int64_t ticks)
{
	struct list_elem *cur;
	struct list_elem *next;
	struct list_elem *end;
	enum intr_level old_level;

	/* sleep_list가 비어있으면 종료 */
	if (list_empty(&sleep_list))
		return;

	cur = list_front(&sleep_list);
	end = list_tail(&sleep_list);
	/* 시간이 다 된 스레드를 sleep_list에서 제거 */
	while (cur != end && list_entry(cur, struct thread, elem)->alarm <= ticks)
	{
		next = list_remove(cur);
		thread_unblock(list_entry(cur, struct thread, elem));
		cur = next;
	}
	thread_check_preemption();
}

bool priority_more(const struct list_elem *elem_prev, const struct list_elem *elem_next, void *aux UNUSED)
{
	struct thread *t_prev;
	struct thread *t_next;

	ASSERT(elem_prev != NULL); // YJ TODO: 만약 list_insert_ordered와 매개변수가 같으면 ASSERT 지웁시다
	ASSERT(elem_next != NULL); // YJ TODO: 만약 list_insert_ordered와 매개변수가 같으면 ASSERT 지웁시다

	t_prev = list_entry(elem_prev, struct thread, elem); // YJ: elem_prev(요소의 주소) - elem(요소의 주소와 스레드의 시작 주소 사이의 거리) = 스레드 시작 주소
	t_next = list_entry(elem_next, struct thread, elem);

	return t_prev->priority > t_next->priority; // YJ: 우리가 반환해야 할 참/거짓값. 이전,이후 스레드의 priority값을 비교해서 담음.
}

/* nuri. 현재 스레드의 기본 우선순위를 변경하고, priority donation을 고려한 유효 우선순위
   다시 계산 일시적인 변경이 아닌, 애초에 우선순위를 변경하고 싶을 때 사용하는 함수
   따라서 이 함수로 test하고자 하는 것은 특정 스레드의 우선순위가 바뀌면, donation
   여부가 달라지는지, donation 받을 스레드보다 우선순위가 높아졌다면 donate 받을 필요
   없으니까 그걸 리스트를 순회하면서 다시 계산하려는 의도를 가진 함수다.
   사용하는 test_case: priority change, priority-donate-lower */
/* YJ TODO: priority-fifo 통과하려면 thread_set_priority()에 선점 검사를 연결해야 함.
priority-fifo는 이런 순서로 진행됨.
1. 현재 스레드 우선순위를 33으로 올린다.
2. 우선순위 32인 스레드 16개를 생성한다. 현재 스레드보다 낮아서 준비 큐에서 기다린다.
3. 현재 스레드 우선순위를 31로 낮춘다.
4. 즉시 CPU를 양보해서, 기다리던 16개가 같은 순서로 반복 실행하도록 한다.**이게 없어서 수정함ㅇㅇ**
현재 thread_set_priority() (pintos/threads/thread.c:350)는 우선순위 값만 바꾸고 계속 실행. 그래서 4번이 빠져 있음.
수정한 함수의 내용.
- 현재 우선순위를 변경한 직후, 기존 thread_check_preemption()으로 양보가 필요한지 검사한다.
*/

/* nuri_MLFQS. thraeed_set_priority의 로직
   1. 현재 스레드의 original_priority를 변경한다.
   2. priority를 기본 우선순위로 초기화 한다.
   3. donations 리스트를 순회한다.
   4. 기부받은 우선순위 중 최댓값을 적용한다.
   5. 선점(preemption)이 필요한지 검사한다.
   MLFQS는 다른 로직을 사용하므로 삭제가 아닌 별도의 로직으로 적용되도록 조건 검사 필요

   MLFQS의 우선순위 계산 공식:
   priority = PRI_MAX - (recent_cpu/4) - (2*nice)*/
void thread_set_priority(int new_priority)
{
	/* nuri_MLFQS. thread_mlfqs가 true일 때는 MLFQS 방식으로 priority를 계산하므로
	   set_priority로 값이 바뀌는 것을 방지해야 함. */
	if (thread_mlfqs == true)
	{
		return;
	}

	struct thread *current = thread_current(); // 현재 running thread 주소를 받음
	enum intr_level old_level = intr_disable();

	/* 기본 우선순위 변경 donation 때문에 일시적으로 바꾸는게 아니라
	   그냥 이 스레드의 우선순위를 변경하려는 의도 */
	current->original_priority = new_priority;
	current->priority = current->original_priority;

	struct list_elem *e = list_begin(&current->donations);

	/* nuri. 자신에게 우선순위를 기부한 스레드를 순회하면서 기부받은 우선순위가
	   바뀐 우선순위보다 높으면 다시 그 우선순위로 변경. 이 때 바뀐 우선순위는 lock_release()
	   에서 원래 우선순위로 재변경 */
	while (e != list_end(&current->donations))
	{
		struct thread *donor =
			list_entry(e, struct thread, donation_elem);

		if (current->priority < donor->priority)
		{
			current->priority = donor->priority;
		}

		e = list_next(e);
	}
	intr_set_level(old_level);
	thread_check_preemption();
}

/* Returns the current thread's priority. */
int thread_get_priority(void)
{
	return thread_current()->priority;
}

/* YJ TODO: 더 높은 우선순위 스레드가 준비됐으면 CPU 양보.
   인터럽트 안에서는 intr_yield_on_return() 사용.

1. 이전 인터럽트 상태를 저장하며 인터럽트를 끈다.
   큐를 읽고 비교하는 도중 타이머가 실행 순서를 바꾸지 못하도록 보호.
2. 준비 큐가 비었으면 이전 상태를 복원하고 종료한다.
   빈 리스트에서 list_front()를 호출하면 안 됨.
3. 맨 앞 요소를 확인하고, 소속 스레드의 주소를 얻는다.
   여기서는 확인만 하니까 list_pop_front()를 사용하면 안 됨.
4. 현재 스레드와 우선순위를 비교한다.
   준비된 스레드가 엄격히 더 높을 때만 양보.
5. 양보 방법을 실행 맥락에 따라 선택한다.
   intr_context()가 참이면 intr_yield_on_return(), 거짓이면 thread_yield()를 사용.
   인터럽트가 꺼져 있다는 것과 핸들러 안이라는 것은 다름.
6. 끝에서 이전 인터럽트 상태를 복원한다.
   thread_yield()로 다른 스레드가 실행돼도, 나중에 돌아오면 이 복원까지 이어서 수행해.

이 함수는 인자와 반환값이 필요 없음. 기존 ready_list와 현재 스레드를 이용하면 됨.
우선 본문부터 작성하고, 검토 후 호출 위치를 연결.*/
void thread_check_preemption(void)
{
	struct thread *t;	  // 준비 큐 맨 앞에서 기다리는 스레드
	struct thread *t_cur; // 지금 CPU에서 실행 중인 스레드

	// 이전 인터럽트 상태 보관.
	enum intr_level old_level;

	// 인터럽트 끄기
	old_level = intr_disable();

	// 준비 큐가 비었는지 확인. 비어있으면 인터럽트 복원 후 종료
	if (list_empty(&ready_list))
	{
		intr_set_level(old_level);
		return;
	}

	// 맨 앞 요소를 제거 없이 확인하여, 요소에서 소속 스레드 찾기
	t = list_entry(list_front(&ready_list), struct thread, elem);

	// 현재 실행 중인 스레드 찾기. 스레드 정보 보관
	t_cur = thread_current();

	// 인터럽트 핸들러 안인지 확인
	if (t->priority > t_cur->priority)
	{
		if (intr_context() == true)
			intr_yield_on_return();
		else
			thread_yield();
	}

	// 이전 상태 복원.
	intr_set_level(old_level);
}

/* YJ: 우선순위 값이 바뀐 READY 스레드를 준비 큐에서 올바른 위치로 옮기는 함수
함수의 입력과 책임
- 입력 t: 우선순위가 이미 변경된 스레드의 주소.
- t->priority: C가 변경해 둔 새 값.
- 함수의 책임: 준비 큐 위치 갱신.
- 반환값: 없음.
- 기존 자료구조·함수 사용: ready_list, t->elem, priority_more(), list_remove(), list_insert_ordered().

1. 이전 인터럽트 상태를 저장하고 인터럽트를 끈다.
2. 전달받은 t가 THREAD_READY인지 확인한다.
3. READY라면 t->elem을 기존 준비 큐 위치에서 제거한다.
4. 같은 요소를 기존 비교 함수로 우선순위에 맞게 재삽입한다.
5. 이전 인터럽트 상태를 복원한다. */
void thread_priority_changed(struct thread *t)
{
	enum intr_level old_level = intr_disable();

	if (t->status == THREAD_READY)
	{
		list_remove(&(t->elem));
		/* 우선순위에 맞게 READY Queue에 재삽입. */
		list_insert_ordered(&ready_list, &(t->elem), priority_more, NULL);
	}

	intr_set_level(old_level);
}

/* 현재 스레드의 nice을 설정합니다. */
void thread_set_nice(int nice)
{
	struct thread *cur = thread_current();
	enum intr_level old_level = intr_disable();

	cur->nice = nice;

	if (thread_mlfqs) /* nuri_MLFQS. MLFQS가 비활성화 된 경우엔 이 함수를 실행할 이유가 없음으로 조건문으로 true일 때만 실행 */
	{
		mlfqs_update_priority(cur);
	}

	intr_set_level(old_level);

	if (thread_mlfqs) /* nuri_MLFQS. MLFQS모드에서만 우선순위 재계산하도록 조건문 추가 */
	{
		thread_check_preemption();
	}
}

/* 현재 스레드의 nice 값을 반환합니다. */
int thread_get_nice(void)
{
	return thread_current()->nice;
}

/* load_avg을 갱신합니다.
 * RUNNING 상태의 스레드 개수 + READY 상태의 스레드 개수를 합하여
 * CPU 경쟁 스레드 수에 포함합니다 (ready_threads)
 *
 * CPU 경쟁 스레드 수를 평균적인 경쟁 규모(가중치 평균)으로 계산하여
 * load_avg를 갱신합니다. */
void thread_update_load_avg(void)
{
	long long new_load_avg;
	long long ready_threads;

	ASSERT(intr_get_level() == INTR_OFF);

	/* CPU 경쟁 스레드 수 집계 */
	ready_threads = 0;
	if (thread_current() && thread_current() != idle_thread)
		ready_threads++;

	for (struct list_elem *t = list_head(&ready_list)->next; t != list_end(&ready_list); t = t->next)
		ready_threads++;

	/* 고정소수점과 EWMA를 이용하여 새 가중치 평균 계산 */
	new_load_avg = load_avg + (ready_threads * F - load_avg) / 60;

	/* 가중치 업데이트 */
	load_avg = new_load_avg;
}

/* load_avg * 100 값을 반환합니다. */
int thread_get_load_avg(void)
{
	return load_avg > 0 ? ((load_avg * 100 + F / 2) / F) : ((load_avg * 100 - F / 2) / F);
}

/* recent_cpu를 갱신합니다.
 *
 * 해당 함수는 새 load_avg 갱신 -> 갱신된 load_avg와 nice값을 바탕으로 recent_cpu 갱신
 * 흐름을 가집니다. RUNNING, READY, BLOCKED 상태 스레드를 모두 포함하여 갱신합니다.
 */
void thread_update_recent_cpu(void)
{
	ASSERT(intr_get_level() == INTR_OFF);

	/* RUNNING 상태 스레드 recent_cpu 갱신 */
	// (((2 * load_avg * F) / (2 * load_avg + 1)) / F) * thread_get_recent_cpu() + thread_get_nice();

	/* READY 상태 스레드 recent_cpu 갱신 */

	/* BLOCKED 상태 스레드 recent_cpu 갱신 */
}

/* recent_cpu * 100 값을 반환합니다. */
int thread_get_recent_cpu(void)
{
	return thread_current()->recent_cpu > 0 ? ((thread_current()->recent_cpu * 100 + F / 2) / F) : ((thread_current()->recent_cpu * 100 - F / 2) / F);
}

/* Idle thread.  Executes when no other thread is ready to run.

The idle thread is initially put on the ready list by
thread_start().  It will be scheduled once initially, at which
point it initializes idle_thread, "up"s the semaphore passed
to it to enable thread_start() to continue, and immediately
blocks.  After that, the idle thread never appears in the
ready list.  It is returned by next_thread_to_run() as a
special case when the ready list is empty. */
static void
idle(void *idle_started_ UNUSED)
{
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current();
	sema_up(idle_started);

	for (;;)
	{
		/* Let someone else run. */
		intr_disable();
		thread_block();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread(thread_func *function, void *aux)
{
	ASSERT(function != NULL);

	intr_enable(); /* The scheduler runs with interrupts off. */
	function(aux); /* Execute the thread function. */
	thread_exit(); /* If function() returns, kill the thread. */
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread(struct thread *t, const char *name, int priority)
{
	ASSERT(t != NULL);
	ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT(name != NULL);

	memset(t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy(t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t)t + PGSIZE - sizeof(void *);
	t->priority = priority;

	/* nuri. priority donation 관련 멤버 초기화 */
	t->original_priority = priority; /* 기부 받기 전의 본인의 우선순위 */
	list_init(&t->donations);		 /* 자신에게 우선순위를 기부하는 스레드들을 리스트 초기화 */
	t->wait_on_lock = NULL;			 /* 처음에는 lock이 없으니 NULL값 */

	t->donation_target = NULL; /* 새로 생성된 스레드는 아직 누구에게도 우선순위를 기부하지 않았으니 NULL로 초기화 */

	t->magic = THREAD_MAGIC;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run(void)
{
	if (list_empty(&ready_list))
		return idle_thread;
	else
		return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void do_iret(struct intr_frame *tf)
{
	__asm __volatile(
		"movq %0, %%rsp\n"
		"movq 0(%%rsp),%%r15\n"
		"movq 8(%%rsp),%%r14\n"
		"movq 16(%%rsp),%%r13\n"
		"movq 24(%%rsp),%%r12\n"
		"movq 32(%%rsp),%%r11\n"
		"movq 40(%%rsp),%%r10\n"
		"movq 48(%%rsp),%%r9\n"
		"movq 56(%%rsp),%%r8\n"
		"movq 64(%%rsp),%%rsi\n"
		"movq 72(%%rsp),%%rdi\n"
		"movq 80(%%rsp),%%rbp\n"
		"movq 88(%%rsp),%%rdx\n"
		"movq 96(%%rsp),%%rcx\n"
		"movq 104(%%rsp),%%rbx\n"
		"movq 112(%%rsp),%%rax\n"
		"addq $120,%%rsp\n"
		"movw 8(%%rsp),%%ds\n"
		"movw (%%rsp),%%es\n"
		"addq $32, %%rsp\n"
		"iretq"
		: : "g"((uint64_t)tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch(struct thread *th)
{
	uint64_t tf_cur = (uint64_t)&running_thread()->tf;
	uint64_t tf = (uint64_t)&th->tf;
	ASSERT(intr_get_level() == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile(
		/* Store registers that will be used. */
		"push %%rax\n"
		"push %%rbx\n"
		"push %%rcx\n"
		/* Fetch input once */
		"movq %0, %%rax\n"
		"movq %1, %%rcx\n"
		"movq %%r15, 0(%%rax)\n"
		"movq %%r14, 8(%%rax)\n"
		"movq %%r13, 16(%%rax)\n"
		"movq %%r12, 24(%%rax)\n"
		"movq %%r11, 32(%%rax)\n"
		"movq %%r10, 40(%%rax)\n"
		"movq %%r9, 48(%%rax)\n"
		"movq %%r8, 56(%%rax)\n"
		"movq %%rsi, 64(%%rax)\n"
		"movq %%rdi, 72(%%rax)\n"
		"movq %%rbp, 80(%%rax)\n"
		"movq %%rdx, 88(%%rax)\n"
		"pop %%rbx\n" // Saved rcx
		"movq %%rbx, 96(%%rax)\n"
		"pop %%rbx\n" // Saved rbx
		"movq %%rbx, 104(%%rax)\n"
		"pop %%rbx\n" // Saved rax
		"movq %%rbx, 112(%%rax)\n"
		"addq $120, %%rax\n"
		"movw %%es, (%%rax)\n"
		"movw %%ds, 8(%%rax)\n"
		"addq $32, %%rax\n"
		"call __next\n" // read the current rip.
		"__next:\n"
		"pop %%rbx\n"
		"addq $(out_iret -  __next), %%rbx\n"
		"movq %%rbx, 0(%%rax)\n" // rip
		"movw %%cs, 8(%%rax)\n"	 // cs
		"pushfq\n"
		"popq %%rbx\n"
		"mov %%rbx, 16(%%rax)\n" // eflags
		"mov %%rsp, 24(%%rax)\n" // rsp
		"movw %%ss, 32(%%rax)\n"
		"mov %%rcx, %%rdi\n"
		"call do_iret\n"
		"out_iret:\n"
		: : "g"(tf_cur), "g"(tf) : "memory");
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status)
{
	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(thread_current()->status == THREAD_RUNNING);
	while (!list_empty(&destruction_req))
	{
		struct thread *victim =
			list_entry(list_pop_front(&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current()->status = status;
	schedule();
}

/* 다음 스레드를 선택하고, 그 스레드를 RUNNING으로 바꾼 뒤,
필요한 문맥 교환을 수행한다. */
static void
schedule(void)
{
	struct thread *curr = running_thread();
	struct thread *next = next_thread_to_run();

	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(curr->status != THREAD_RUNNING);
	ASSERT(is_thread(next));

	/* 다음 스레드의 상태를 RUNNING으로 변경한다. */
	next->status = THREAD_RUNNING;

	/* 스레드 실행 시작 시점을 초기화한다. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate(next);
#endif

	if (curr != next)
	{
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread)
		{
			ASSERT(curr != next);
			list_push_back(&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch(next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid(void)
{
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire(&tid_lock);
	tid = next_tid++;
	lock_release(&tid_lock);

	return tid;
}
