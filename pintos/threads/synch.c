/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* nuri. list_max()에서 사용할 스레드 우선순위 비교 함수.
   1. list_max()가 받는 인자 list_less_func이 true면 a를 b보다 작은 원소로 판단한다.
   따라서 우선순위가 낮은 스레드를 작은 원소로 취급하도록 구현한 것이다.
   2. list_entry를 통해 list_elem이 속한 thread의 주소를 얻고,
   두 스레드의 priority를 비교한다.
   3. t_a->priority < t_b->priority면 true를 반환하므로,
   list_max()는 최종적으로 우선순위가 가장 높은 스레드의 list_elem을 반환한다.
   con) list_max()가 우선순위가 가장 높은 스레드를 선택하도록, 우선순위가 낮은
   스레드를 작은 원소로 판단하는 비교 함수. */
/* nuri. thread.c의 priority_more 함수와 기능이 겹치므로 주석 처리 후 priority_more 함수 사용 */
// static bool
// thread_priority_less(const struct list_elem *a,
// 					 const struct list_elem *b,
// 					 void *aux) {
// 	struct thread *t_a = list_entry(a, struct thread, elem);
// 	struct thread *t_b = list_entry(b, struct thread, elem);

// 	return (t_a->priority < t_b->priority);
// }

/* nuri. lock_acquire()에서 사용할 중첩(연쇄) priority donation을 처리하는 함수.
   1. current가 다른 스레드가 소유한 lock을 기다리는 경우,
   current의 priority를 해당 lock 소유자에게 기부하는 역할
   2. lock의 소유자도 다른 lock을 기다리고 있다면,
   그 lock의 소유자에게까지 기부를 연쇄적으로 수행한다.
   3. 각 단계에서 기존 우선순위보다 기부할 우선순위가 높은 경우에만
   priority를 갱신하며, original_priority는 변경하지 않는다. */
static void
donate_priority(struct thread *current)
{
	/* current가 기다리는 lock이 없으면 기부할 필요가 없으니 바로 return */
	if (current->wait_on_lock == NULL)
		return;

	/* current가 받아야하는 lock을 가진 소유자 */
	struct thread *holder = current->wait_on_lock->holder;

	/* nuri. [P2] "순환 기부가 인터럽트를 끈 채 끝없이 돕니다."
	   순환 기부가 발생하면 holder가 NULL이 되지 않아 무한 루프할 수 있으므로
	   기부 깊이를 최대 8단계로 제한한다. 다만 이는 무한 루프를 방지하는 것이지
	   스레드 간 lock 교착 상태 자체는 해결하지 못한다.  */
	int count = 0;
	while (holder != NULL && count < 8)
	{
		if (current->priority > holder->priority)
		{
			holder->priority = current->priority;
			thread_priority_changed(holder);
		}

		/* holder가 다른 lock을 필요로 하지 않으면 기부 종료 */
		if (holder->wait_on_lock == NULL)
			break;

		/* 현재 holder가 받으려고 기다리는 lock의 소유자가 없다면 기부 종료 */
		if (holder->wait_on_lock->holder == NULL)
			break;

		/* 현재 holder가 받으려고 기다리는 lock의 소유자를 다음 priority donation 대상으로 설정 */
		holder = holder->wait_on_lock->holder;
		count++;
	}
}

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
   decrement it.

   - up or "V": increment the value (and wake up one waiting
   thread, if any). */
void sema_init(struct semaphore *sema, unsigned value)
{
	ASSERT(sema != NULL);

	sema->value = value;
	list_init(&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. This is
   sema_down function. */

/* 세마포어의 권한을 하나 획득하는 함수.
	허가가 없으면 현재 스레드를 BLOCKED 상태로 전환하고, 획득할 수 있을 때까지 대기한다.
	1. sema->value가 0이면 사용 가능한 자원이 없으므로 현재 스레드를 대기 리스트에 추가하고
	BLOCKED 상태로 전환한다.
	2. 다른 스레드가 sema_up()을 호출하여 깨워주면 사용 가능한 자원이 있는지 while문에서 검사
	3. sema->value가 0보다 크면 1 감소시키고, 현재 스레드가 사용 권한을 획득한다.*/
void sema_down(struct semaphore *sema)
{
	enum intr_level old_level;

	ASSERT(sema != NULL);
	ASSERT(!intr_context());
	old_level = intr_disable();

	/* 사용 가능한 자원이 없으면 sema->waiters에 추가하고 BLOCKED 상태로 전환 */
	while (sema->value == 0)
	{
		struct thread *current = thread_current();

		/* nuri. [P1] "락을 넘겨받은 새 소유자에게 남은 대기자의 기부가 이어지지 않습니다."
		   lock을 기다리던 스레드가 꺠어나도 다른 스레드가 먼저 lock을 획득하면 다시 BLOCKED 되는 경우의 수 발생.
		   이때 lock 소유자가 바뀌었을 수도 있으므로 기부 대상 확인 */
		if (current->wait_on_lock != NULL &&
			sema == &current->wait_on_lock->semaphore &&
			current->wait_on_lock->holder != NULL)
		{

			/* 현재 기다리는 lock을 실제로 소유한 스레드 */
			struct thread *holder = current->wait_on_lock->holder;

			/* 이전에 기부하던 스레드와 현재 lock 소유자가 다르면
			   이전 소유자의 donations 리스트에서 current를 제거 */
			if (current->donation_target != NULL &&
				current->donation_target != holder)
			{
				list_remove(&current->donation_elem);
				current->donation_target = NULL;
			}

			/* current가 현재 lock 소유자의 donations 리스트에 없다면
			   새 소유자에게 우선순위를 기부할 수 있도록 등록 */
			if (current->donation_target == NULL)
			{
				list_push_back(&holder->donations, &current->donation_elem);
				current->donation_target = holder;
			}

			/* 새 소유자에게 current의 우선순위를 반영하고
			   새 소유자도 다른 lock을 기다린다면 중첩 기부 처리 */
			donate_priority(current);
		}
		list_push_back(&sema->waiters, &thread_current()->elem);
		thread_block();
	}

	/* 사용 가능한 자원 하나를 획득했음을 의미. */
	sema->value--;
	intr_set_level(old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool sema_try_down(struct semaphore *sema)
{
	enum intr_level old_level;
	bool success;

	ASSERT(sema != NULL);

	old_level = intr_disable();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level(old_level);

	return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */

/* nuri. 사용 권한을 반환하고 다음 스레드가 사용할 수 있게 하는 함수.
   기존 방식은 FIFO여서 우선순위가 높은 순으로 깨우기 위해 변경
   다만 우선순위 높은 순으로 정렬한 것은 아니고 우선순위 높은 쓰레드를 탐색 후 깨운 것*/
void sema_up(struct semaphore *sema)
{
	enum intr_level old_level;
	struct thread *max_thread = NULL;

	ASSERT(sema != NULL);

	old_level = intr_disable();
	if (!list_empty(&sema->waiters))
	{
		struct list_elem *max_elem;

		max_elem = list_min(&sema->waiters, priority_more, NULL);
		list_remove(max_elem);

		// max_elem이 있는 thread의 주소를 얻은 후, ready list로 이동
		max_thread = list_entry(max_elem, struct thread, elem);
		thread_unblock(max_thread);
	}
	sema->value++;

	intr_set_level(old_level);

	/* nuri. ready 리스트의 최고 우선순위 스레드와 현재 스레드를 비교하여 더 높은
	   우선순위 스레드에게 CPU 양보 */
	thread_check_preemption();
}

static void sema_test_helper(void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void sema_self_test(void)
{
	struct semaphore sema[2];
	int i;

	printf("Testing semaphores...");
	sema_init(&sema[0], 0);
	sema_init(&sema[1], 0);
	thread_create("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up(&sema[0]);
		sema_down(&sema[1]);
	}
	printf("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper(void *sema_)
{
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down(&sema[0]);
		sema_up(&sema[1]);
	}
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void lock_init(struct lock *lock)
{
	ASSERT(lock != NULL);

	lock->holder = NULL;
	sema_init(&lock->semaphore, 1);
	list_init(&lock->waiting_threads);
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void lock_acquire(struct lock *lock)
{
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(!lock_held_by_current_thread(lock));

	struct thread *current = thread_current();

	enum intr_level old_level = intr_disable(); /* 원자성 확보를 위한 intr 비활성화 */
	struct thread *holder = lock->holder;		// 현재 lock을 소유한 스레드의 주소

	/* nuri.LOCK 소유자가 있다면 priority Donation 처리
	   우선순위가 높은 스레드가 낮은 스레드의 Lock을 획득하려고 할 때,
	   sema_down을 호출하면 H는 BLOCKED 상태가 됨 따라서 priority donation 불가능
	   그래서 낮은 스레드의 우선순위를 높여서 Lock을 realese 할 수 있게 만들어야 함.*/

	/* 다른 스레드가 LOCK을 소유하고 있다면 */
	if (holder != NULL)
	{

		current->wait_on_lock = lock;

		/* nuri. [P1] "락을 넘겨받은 새 소유자에게 남은 대기자의 기부가 이어지지 않습니다."
		   lock 소유자가 변경되어도 우선순위 기부가 누락되지 않도록
		   대기 스레드를 별도로 관리*/
		list_push_back(&lock->waiting_threads,
					   &current->waiting_elem);

		list_push_back(&holder->donations,
					   &current->donation_elem);

		/* nuri. [P1] "락을 넘겨받은 새 소유자에게 남은 대기자의 기부가 이어지지 않습니다."
		   현재 스레드가 어느 lock 소유자에게 우선순위를 기부하는지 기록 */
		current->donation_target = holder;

		/* nuri. 중첩 priority donation */
		donate_priority(current);
	}

	sema_down(&lock->semaphore);

	/* nuri.[P1] "락을 넘겨받은 새 소유자에게 남은 대기자의 기부가 이어지지 않습니다."
	   lock 획득에 성공했으므로 대기자 목록에서 제거
	   list_remove()는 실제 리스트에 연결된 노드에만 호출해야 함.
	   처음부터 lock이 비어 있었다면 waiting_elkem을 등록하지 않은 것이므로 제거하면 안됨. */
	if (holder != NULL)
	{
		list_remove(&current->waiting_elem);
	}

	current->wait_on_lock = NULL; /* 더 이상 기다리는 lock이 없음 */
	lock->holder = current;		  /* lock 소유자 등록 */

	/* nuri.[P1] "락을 넘겨받은 새 소유자에게 남은 대기자의 기부가 이어지지 않습니다."
	   lock의 소유자가 변경되었으므로 남은 대기자들의 기부 관계를 새 소유자에게 이전
	   waiter: 아직 lock을 획득하지 못한 대기 스레드이자 우선순위를 기부할 스레드
	   current: lock을 새로 획득한 스래드이자 기부 받는 대상 */
	struct list_elem *e = list_begin(&lock->waiting_threads);

	while (e != list_end(&lock->waiting_threads))
	{

		/* waiting_elem을 통해 실제 대기 스레드의 주소를 가져옴 */
		struct thread *waiter = list_entry(e, struct thread, waiting_elem);

		/* 대기 스레드가 이미 기부 중이고, 그 대상이 현재 lock의 새 소유자와 다른지 확인 */
		if (waiter->donation_target != NULL &&
			waiter->donation_target != current)
		{
			list_remove(&waiter->donation_elem); // 이전 lock 소유자에게 기부하던 연결 제거
			waiter->donation_target = NULL;		 // 더 이상 이전 소유자의 기부자로 등록되어 있지 않음을 NULL로 표시
		}

		/* waiter가 아직 새 lock 소유자의 기부자로 등록되지 않은 경우 */
		if (waiter->donation_target == NULL)
		{
			list_push_back(&current->donations, &waiter->donation_elem); // waiter를 새 lock 소유자의 기부자 목록에 연결
			waiter->donation_target = current;
		}
		e = list_next(e);
	}

	/* nuri. [p1] 새 lock 소유자가 남은 대기자들에게 기부받은 우선순위 반영 lock_release()에 있는
	   우선순위 재계산 로직이 그대로 반복되기 때문에 별도의 함수로 만들어서 처리할 수도 있으나 우선
	   내부에 반복문으로 작성 */
	current->priority = current->original_priority;

	e = list_begin(&current->donations);

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
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool lock_try_acquire(struct lock *lock)
{
	bool success;

	ASSERT(lock != NULL);
	ASSERT(!lock_held_by_current_thread(lock));

	success = sema_try_down(&lock->semaphore);
	if (success)
		lock->holder = thread_current();
	return success;
}

/* Releases LOCK, which must be owned by the current thread.
   This is lock_release function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void lock_release(struct lock *lock)
{
	ASSERT(lock != NULL);
	ASSERT(lock_held_by_current_thread(lock));

	struct thread *current = thread_current();
	enum intr_level old_level = intr_disable();

	/* 해제하는 lock과 관련된 기부자 제거 */
	struct list_elem *e = list_begin(&current->donations);

	while (e != list_end(&current->donations))
	{
		struct thread *donor = list_entry(e, struct thread, donation_elem);

		if (donor->wait_on_lock == lock)
		{
			e = list_remove(e);

			/* [P1] "락을 넘겨받은 새 소유자에게 남은 대기자의 기부가 이어지지 않습니다."
			기부자 리스트에서 제거했으므로 기존 기부 대상 정보도 초기화 */
			donor->donation_target = NULL;
		}
		else
		{
			e = list_next(e);
		}
	}

	/* 원래 가지고 있는 우선순위로 초기화*/
	current->priority = current->original_priority;

	e = list_begin(&current->donations);

	while (e != list_end(&current->donations))
	{
		struct thread *donor = list_entry(e, struct thread, donation_elem);

		if (current->priority < donor->priority)
		{
			current->priority = donor->priority;
		}

		e = list_next(e);
	}

	lock->holder = NULL;
	sema_up(&lock->semaphore); // nuri. sema_up에서 thread_check_preemptio() 호출했으므로 여기서 추가 호출 필요 없음.
	intr_set_level(old_level);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool lock_held_by_current_thread(const struct lock *lock)
{
	ASSERT(lock != NULL);

	return lock->holder == thread_current();
}

/* One semaphore in a list. */
struct semaphore_elem
{
	struct list_elem elem;		  /* List element. */
	struct semaphore semaphore;	  /* This semaphore. */
	struct thread *waiter_thread; /* 조건 변수에서 대기 중인 스레드의 주소를
									 저장하기 위해 추가한 포인터*/
};

/* nuri. cond_signal()에서 사용할 조건 변수 대기 스레드의 우선순위 비교 함수.
   1. cond->waiter에는 semaphore_elem이 저장되어 있으므로,
   list_entry()를 통해 각 semaphore_elem의 주소를 얻는다.
   2. waiter_thread는 대기 중인 스레드를 가리키며, 해당 스레드의 priority를 비교한다.
   3. s_a의 priority가 s_b보다 낮으면 true를 반환하므로, list_max()는 최종적으로
   우선순위가 가장 높은 대기자의 list_elem을 반환한다. */
static bool
cond_priority_less(const struct list_elem *a,
				   const struct list_elem *b,
				   void *aux UNUSED)
{
	struct semaphore_elem *s_a = list_entry(a, struct semaphore_elem, elem);
	struct semaphore_elem *s_b = list_entry(b, struct semaphore_elem, elem);

	return (s_a->waiter_thread->priority < s_b->waiter_thread->priority);
}

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void cond_init(struct condition *cond)
{
	ASSERT(cond != NULL);

	list_init(&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */

/* nuri. 조건 변수의 신호를 받을때까지 현재 스레드를 대기시키는 함수
   1. 현재 스레드를 조건 변수의 대기 리스트에 등록시킨다.
   2. lock을 해제한 후, 내부 세마포어에서 신호를 기다린다.
   3. 신호를 받아 깨어나면 lock을 다시 획득한 후 반환한다.
   *Mesa 방식이므로 cond_signal()은 대기 스레드를 꺠울 뿐,
   cpu 실행 권한이나 lock 소유권을 직접 넘겨주지는 않는다. */
void cond_wait(struct condition *cond, struct lock *lock)
{
	struct semaphore_elem waiter; // 현재 대기를 관리할 지역 변수

	ASSERT(cond != NULL);
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(lock_held_by_current_thread(lock));

	sema_init(&waiter.semaphore, 0);			  // 내부 세마포어 초기화 후 0으로 설정
	waiter.waiter_thread = thread_current();	  // 조건 변수 대기자 간 우선순위 비교를 위해 현재 스레드의 주소 저장
	list_push_back(&cond->waiters, &waiter.elem); // 조건 변수 대기 리스트에 등록

	/* Mesa 방식의 조건 변수 대기
	   cond_signal()로 스레드를 꺠우더라도, 신호를 보낸 스레드에서
	   대기하던 스레드로 실행 권한과 lock 소유권이 원자적으로 넘어
	   가지 않는 조건 변수 방식
	   sema_down()에서 깨어나는 것과 lock_acquire()는 별개 */

	lock_release(lock);			  // 다른 스레드가 공유 자원에 접근하도록 lock 해제
	sema_down(&waiter.semaphore); // cond_signal()의 신호를 받을때까지 대기
	lock_acquire(lock);			  // lock을 다시 획득 후 cond_wait()에서 반환
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */

/* nuri. 조건 변수 대기자 중 유효 우선순위가 가장 높은 스레드를 꺠우는 함수.
   1. cond_priority_less()로 대기자들의 우선순위를 비교하고,
   2. sema_up을 통해 선택한 우선순위가 가장 높은 스레드를 깨운다. */
void cond_signal(struct condition *cond, struct lock *lock UNUSED)
{
	ASSERT(cond != NULL);
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(lock_held_by_current_thread(lock));

	if (!list_empty(&cond->waiters))
	{
		struct list_elem *max_elem;

		/* 우선순위 가장 높은 스레드 탐색 후 elem의 주소 반환 */
		max_elem = list_max(&cond->waiters, cond_priority_less, NULL);

		/* cond->waiters 리스트에서 제거 */
		list_remove(max_elem);

		/* max_elem에서 semaphore_elem의 주소를 얻음*/
		struct semaphore_elem *waiter =
			list_entry(max_elem, struct semaphore_elem, elem);

		/* 내부 세마포어를 통해 대기 스레드를 깨우고, 필요하면 preemption */
		sema_up(&waiter->semaphore);
	}
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void cond_broadcast(struct condition *cond, struct lock *lock)
{
	ASSERT(cond != NULL);
	ASSERT(lock != NULL);

	while (!list_empty(&cond->waiters))
		cond_signal(cond, lock);
}
