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

/* nuri. list_max()에서 우선도를 비교할 비교 함수 구현
   'max'니까 작은 쪽을 false로 반환하도록 설계
   고로 우선순위가 낮은 스레드를 작은 원소로 취급해야함. */
static bool
thread_priority_less(const struct list_elem *a,
					 const struct list_elem *b,
					 void *aux) {
	struct thread *t_a = list_entry(a, struct thread, elem);
	struct thread *t_b = list_entry(b, struct thread, elem);

	return (t_a->priority < t_b->priority);
}

/* nuri. 우선순위 기부 함수를 별도 구현 */
static void
donate_priority(struct thread *current) {
	if (current->wait_on_lock == NULL) {
		return;
	}

	struct thread *holder = current->wait_on_lock->holder;

	while (holder != NULL) {
		if (current->priority > holder->priority) {
			holder->priority = current->priority;
		}

		if (holder->wait_on_lock == NULL) {
			break;
		}

		if (holder->wait_on_lock->holder == NULL) {
			break;
		}

		holder = holder->wait_on_lock->holder;
	}
}

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
   decrement it.

   - up or "V": increment the value (and wake up one waiting
   thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL);

	sema->value = value;
	list_init (&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. This is
   sema_down function. */

/* 사용 권한을 획득하기 위한 함수.*/
void
sema_down (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);
	ASSERT (!intr_context ());

	old_level = intr_disable ();
	// 하술된 while문은 깨어난 스레드가 실제로 자원을 획득할 수 있는지 다시 확인하는 역할
	while (sema->value == 0) {
		list_push_back (&sema->waiters, &thread_current ()->elem);
		thread_block ();
	}
	sema->value--;
	intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) {
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

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
/* nuri. 사용 권한을 반환하고 다음 스레드가 사용할 수 있게 하는 함수. 
   기존 방식은 FIFO여서 우선순위가 높은 순으로 깨우기 위해 변경
   다만 우선순위 높은 순으로 정렬한 것은 아니고 우선순위 높은 쓰레드를 탐색 후 깨운 것*/
void
sema_up (struct semaphore *sema) {
	enum intr_level old_level;
	struct thread *max_thread = NULL;
	bool should_preempt = false; // CPU를 양보해야 하는지 저장하는 변수

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (!list_empty (&sema->waiters)) {
		struct list_elem *max_elem;
		
		max_elem = list_max(&sema->waiters, thread_priority_less, NULL);
		list_remove(max_elem);

		//max_elem이 있는 thread의 주소를 얻은 후, ready list로 이동
		max_thread = list_entry(max_elem, struct thread, elem);
		thread_unblock(max_thread);
	}
	sema->value++;

	should_preempt = (max_thread != NULL &&
					  max_thread->priority > thread_current()->priority);
	intr_set_level (old_level);

	/* nuri. what if? 아래 조건문에 도달하기 전에 타이머 인터럽트가 발생한다면?
	   scenario: 우선순위 20의 A, 우선순위 50의 B 쓰레드 가정
	   1. A가 sema_up() 실행
	   2. thread_unblock(B)로 B가 ready 상태가 됨
	   3. should_preempt = TRUE가 저장
	   4. A가 intr_set_level(old_level)로 인터럽트 활성화
	   5. 바로 이 순간 타이머 인터럽트 발생
	   6. 그래서 ready list에 있던 B가 running됨
	   7. 이후 다시 A가 실행되면 if (should_preempt)부터 실행
	   8. B는 이미 실행을 마쳤거나, blocked 상태일 수도 있는데 A의 should_preempt는 True인 상황
	   Con) A가 불필요하게 thread_yield()호출 가능성 있음 */

	/* nuri. 우선순위가 높은 쓰레드를 깨웠는데, 현재 실행 중인 쓰레드가 우선순위가 낮다면
	   CPU를 양보할 수 있는 조건문
	   thread_check_preemption()함수 쓰게될지도 모름 의논해봐야 함*/
	if (should_preempt) {
		if (intr_context()) {
			intr_yield_on_return();
		}
		else {
			thread_yield();
		}
	}
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) {
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

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) {
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down (&sema[0]);
		sema_up (&sema[1]);
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
void
lock_init (struct lock *lock) {
	ASSERT (lock != NULL);

	lock->holder = NULL;
	sema_init (&lock->semaphore, 1);
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (!lock_held_by_current_thread (lock));

	struct thread *current = thread_current();

	enum intr_level old_level = intr_disable(); /* 원자성 확보를 위한 intr 비활성화 */
	struct thread *holder = lock->holder; // 현재 lock을 소유한 스레드의 주소

	/* nuri.LOCK 소유자가 있다면 priority Donation 처리
	   우선순위가 높은 스레드가 낮은 스레드의 Lock을 획득하려고 할 때,
	   sema_down을 호출하면 H는 BLOCKED 상태가 됨 따라서 priority donation 불가능
	   그래서 낮은 스레드의 우선순위를 높여서 Lock을 realese 할 수 있게 만들어야 함.*/

	/* 다른 스레드가 LOCK을 소유하고 있다면 */
	if (holder != NULL) {

		current->wait_on_lock = lock;

		list_push_back(&holder->donations,
					   &current->donation_elem);

		/* nuri. 중첩 priority donation */
		donate_priority(current);
		
	}

	sema_down (&lock->semaphore);
	current->wait_on_lock = NULL; /* 더 이상 기다리는 lock이 없음 */
	lock->holder = current; /* lock 소유자 등록 */

	intr_set_level(old_level);
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock) {
	bool success;

	ASSERT (lock != NULL);
	ASSERT (!lock_held_by_current_thread (lock));

	success = sema_try_down (&lock->semaphore);
	if (success)
		lock->holder = thread_current ();
	return success;
}

/* Releases LOCK, which must be owned by the current thread.
   This is lock_release function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (lock_held_by_current_thread (lock));

	struct thread *current = thread_current();
	enum intr_level old_level = intr_disable();


	/* 해제하는 lock과 관련된 기부자 제거 */
	struct list_elem *e = list_begin(&current->donations);

	while (e != list_end(&current->donations)) {
		struct thread *donor = 
			list_entry(e, struct thread, donation_elem);
		
		if (donor->wait_on_lock == lock) {
			e = list_remove(e);
		}
		else {
			e = list_next(e);
		}
	}

	/* 원래 가지고 있는 우선순위로 초기화*/
	current->priority = current->original_priority;

	e = list_begin(&current->donations);

	while (e != list_end(&current->donations)) {
		struct thread *donor =
			list_entry(e, struct thread, donation_elem);
		
		if (current->priority < donor->priority) {
			current->priority = donor->priority;
		}

		e = list_next(e);
	}

	lock->holder = NULL;
	sema_up (&lock->semaphore);
	intr_set_level(old_level);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) {
	ASSERT (lock != NULL);

	return lock->holder == thread_current ();
}

/* One semaphore in a list. */
struct semaphore_elem {
	struct list_elem elem;              /* List element. */
	struct semaphore semaphore;         /* This semaphore. */
	struct thread *waiter_thread;		/* 조건 변수에서 대기 중인 스레드의 주소를
										   저장하기 위해 추가한 포인터*/
};

// /* nuri. 조건 변수의 우선도를 비교할 비교함수 구현*/
// static bool
// cond_priority_less(const struct list_elem *a,
// 				   const struct list_elem *b,
// 				   void *aux) {
// 	struct semaphore_elem *s_a = list_entry(a, struct semaphore_elem, elem);
// 	struct semaphore_elem *s_b = list_entry(b, struct semaphore_elem, elem);
// 	struct thread *t_a = NULL;
// 	struct thread *t_b = NULL;

// 	if (!list_empty(&s_a->semaphore.waiters)) {
// 		t_a = list_entry(list_begin(&s_a->semaphore.waiters),
// 						struct thread, elem);
// 	}

// 	if (!list_empty(&s_b->semaphore.waiters)) {
// 		t_b = list_entry(list_begin(&s_b->semaphore.waiters),
// 						struct thread, elem);
// 	}
	
// }

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL);

	list_init (&cond->waiters);
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
void
cond_wait (struct condition *cond, struct lock *lock) {
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

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	if (!list_empty (&cond->waiters))
		sema_up (&list_entry (list_pop_front (&cond->waiters),
					struct semaphore_elem, elem)->semaphore);
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);

	while (!list_empty (&cond->waiters))
		cond_signal (cond, lock);
}
