/*
 * kernel/locking/mutex.c
 *
 * Mutexes: blocking mutual exclusion locks
 *
 * Started by Ingo Molnar:
 *
 *  Copyright (C) 2004, 2005, 2006 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *
 * Many thanks to Arjan van de Ven, Thomas Gleixner, Steven Rostedt and
 * David Howells for suggestions and improvements.
 *
 *  - Adaptive spinning for mutexes by Peter Zijlstra. (Ported to mainline
 *    from the -rt tree, where it was originally implemented for rtmutexes
 *    by Steven Rostedt, based on work by Gregory Haskins, Peter Morreale
 *    and Sven Dietrich.
 *
 * Also see Documentation/locking/mutex-design.txt.
 */
#include <linux/mutex.h>
#include <linux/ww_mutex.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/export.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/debug_locks.h>
#include "mcs_spinlock.h"

/*
 * In the DEBUG case we are using the "NULL fastpath" for mutexes,
 * which forces all calls into the slowpath:
 */
#ifdef CONFIG_DEBUG_MUTEXES
# include "mutex-debug.h"
# include <asm-generic/mutex-null.h>
/*
 * Must be 0 for the debug case so we do not do the unlock outside of the
 * wait_lock region. debug_mutex_unlock() will do the actual unlock in this
 * case.
 */
# undef __mutex_slowpath_needs_to_unlock
# define  __mutex_slowpath_needs_to_unlock()	0
#else
# include "mutex.h"
# include <asm/mutex.h>
#endif

void
__mutex_init(struct mutex *lock, const char *name, struct lock_class_key *key)
{
	atomic_set(&lock->count, 1);
	spin_lock_init(&lock->wait_lock);
	INIT_LIST_HEAD(&lock->wait_list);
	mutex_clear_owner(lock);
#ifdef CONFIG_MUTEX_SPIN_ON_OWNER
	osq_lock_init(&lock->osq);
#endif

	debug_mutex_init(lock, name, key);
}

EXPORT_SYMBOL(__mutex_init);

#ifndef CONFIG_DEBUG_LOCK_ALLOC
/*
 * We split the mutex lock/unlock logic into separate fastpath and
 * slowpath functions, to reduce the register pressure on the fastpath.
 * We also put the fastpath first in the kernel image, to make sure the
 * branch is predicted by the CPU as default-untaken.
 */
__visible void __sched __mutex_lock_slowpath(atomic_t *lock_count);

/**
 * mutex_lock - acquire the mutex
 * @lock: the mutex to be acquired
 *
 * Lock the mutex exclusively for this task. If the mutex is not
 * available right now, it will sleep until it can get it.
 *
 * The mutex must later on be released by the same task that
 * acquired it. Recursive locking is not allowed. The task
 * may not exit without first unlocking the mutex. Also, kernel
 * memory where the mutex resides must not be freed with
 * the mutex still locked. The mutex must first be initialized
 * (or statically defined) before it can be locked. memset()-ing
 * the mutex to 0 is not allowed.
 *
 * ( The CONFIG_DEBUG_MUTEXES .config option turns on debugging
 *   checks that will enforce the restrictions and will also do
 *   deadlock debugging. )
 *
 * This function is similar to (but not equivalent to) down().
 */
void __sched mutex_lock(struct mutex *lock)
{

/* IAMROOT-12A:
 * ------------
 * CONFIG_PREEMPT_VOLUNTARY(preempt points를 동작시킨다) 커널 옵션을 사용하는 경우 
 * 상황에 따라 reschedule 될 수 있다. (즉 sleep)
 */
	might_sleep();
	/*
	 * The locking fastpath is the 1->0 transition from
	 * 'unlocked' into 'locked' state.
	 */

/* IAMROOT-12A:
 * ------------
 * fastpath는 unlock(1) 상태에서 0(lock) 상태로 변경을 시도한다.
 * 안되면 __mutex_lock_slowpath()를 호출한다.
 */
	__mutex_fastpath_lock(&lock->count, __mutex_lock_slowpath);
	mutex_set_owner(lock);
}

EXPORT_SYMBOL(mutex_lock);
#endif

static __always_inline void ww_mutex_lock_acquired(struct ww_mutex *ww,
						   struct ww_acquire_ctx *ww_ctx)
{
#ifdef CONFIG_DEBUG_MUTEXES
	/*
	 * If this WARN_ON triggers, you used ww_mutex_lock to acquire,
	 * but released with a normal mutex_unlock in this call.
	 *
	 * This should never happen, always use ww_mutex_unlock.
	 */
	DEBUG_LOCKS_WARN_ON(ww->ctx);

	/*
	 * Not quite done after calling ww_acquire_done() ?
	 */
	DEBUG_LOCKS_WARN_ON(ww_ctx->done_acquire);

	if (ww_ctx->contending_lock) {
		/*
		 * After -EDEADLK you tried to
		 * acquire a different ww_mutex? Bad!
		 */
		DEBUG_LOCKS_WARN_ON(ww_ctx->contending_lock != ww);

		/*
		 * You called ww_mutex_lock after receiving -EDEADLK,
		 * but 'forgot' to unlock everything else first?
		 */
		DEBUG_LOCKS_WARN_ON(ww_ctx->acquired > 0);
		ww_ctx->contending_lock = NULL;
	}

	/*
	 * Naughty, using a different class will lead to undefined behavior!
	 */
	DEBUG_LOCKS_WARN_ON(ww_ctx->ww_class != ww->ww_class);
#endif
	ww_ctx->acquired++;
}

/*
 * After acquiring lock with fastpath or when we lost out in contested
 * slowpath, set ctx and wake up any waiters so they can recheck.
 *
 * This function is never called when CONFIG_DEBUG_LOCK_ALLOC is set,
 * as the fastpath and opportunistic spinning are disabled in that case.
 */
static __always_inline void
ww_mutex_set_context_fastpath(struct ww_mutex *lock,
			       struct ww_acquire_ctx *ctx)
{
	unsigned long flags;
	struct mutex_waiter *cur;

	ww_mutex_lock_acquired(lock, ctx);

	lock->ctx = ctx;

	/*
	 * The lock->ctx update should be visible on all cores before
	 * the atomic read is done, otherwise contended waiters might be
	 * missed. The contended waiters will either see ww_ctx == NULL
	 * and keep spinning, or it will acquire wait_lock, add itself
	 * to waiter list and sleep.
	 */
	smp_mb(); /* ^^^ */

	/*
	 * Check if lock is contended, if not there is nobody to wake up
	 */
	if (likely(atomic_read(&lock->base.count) == 0))
		return;

	/*
	 * Uh oh, we raced in fastpath, wake up everyone in this case,
	 * so they can see the new lock->ctx.
	 */
	spin_lock_mutex(&lock->base.wait_lock, flags);
	list_for_each_entry(cur, &lock->base.wait_list, list) {
		debug_mutex_wake_waiter(&lock->base, cur);
		wake_up_process(cur->task);
	}
	spin_unlock_mutex(&lock->base.wait_lock, flags);
}

/*
 * After acquiring lock in the slowpath set ctx and wake up any
 * waiters so they can recheck.
 *
 * Callers must hold the mutex wait_lock.
 */
static __always_inline void
ww_mutex_set_context_slowpath(struct ww_mutex *lock,
			      struct ww_acquire_ctx *ctx)
{
	struct mutex_waiter *cur;

	ww_mutex_lock_acquired(lock, ctx);
	lock->ctx = ctx;

	/*
	 * Give any possible sleeping processes the chance to wake up,
	 * so they can recheck if they have to back off.
	 */
	list_for_each_entry(cur, &lock->base.wait_list, list) {
		debug_mutex_wake_waiter(&lock->base, cur);
		wake_up_process(cur->task);
	}
}

#ifdef CONFIG_MUTEX_SPIN_ON_OWNER
static inline bool owner_running(struct mutex *lock, struct task_struct *owner)
{
/* IAMROOT-12AB:
 * ------------
 * lock->owner: 현재 시점의 락의 owner 태스크
 *        (이 값은 기존 owner가 unlock 하게되는 순간에 null로 바뀔 수 있음)
 * owner: spin 중 매 번 lock->owner를 읽어온 값
 *	  (이 값은 null이 아닌 상태에서 이 함수에 진입되었다.)
 *
 * 아래 조건 처럼 뮤텍스의 owner 유지되고 있지 않으면 실패.
 */
	if (lock->owner != owner)
		return false;

	/*
	 * Ensure we emit the owner->on_cpu, dereference _after_ checking
	 * lock->owner still matches owner, if that fails, owner might
	 * point to free()d memory, if it still matches, the rcu_read_lock()
	 * ensures the memory stays valid.
	 */
	barrier();

/* IAMROOT-12AB:
 * ------------
 * owner가 running 중인지 아닌지를 리턴
 *     0=lock owner가 preemption이 된 경우(not running)
 *     1=lock owner가 여전히 running 중
 */
	return owner->on_cpu;
}

/*
 * Look out! "owner" is an entirely speculative pointer
 * access and not reliable.
 */

/* IAMROOT-12AB:
 * ------------
 * 성공: spin이 정상적으로 완료된 경우(lock->owner가 null이 된 순간)
 *       (owner 태스크에서 unlock된 순간 이므로 midpath 성공 요건을 갖춤).
 * 실패: 리스케쥴 요청이 있는 경우 루프를 중단.
 */
static noinline
int mutex_spin_on_owner(struct mutex *lock, struct task_struct *owner)
{
/* IAMROOT-12AB:
 * ------------
 * 뮤텍스의 onwer(midpath에 들어왔을 때)가 러닝중에 있으면 루프를 돌며 기다린다.
 * 스케쥴 조정 요청이 있는 경우 spin(midpath)을 포기하고 빠져나간다.
 */
	rcu_read_lock();
	while (owner_running(lock, owner)) {
		if (need_resched())
			break;

		cpu_relax_lowlatency();
	}
	rcu_read_unlock();

	/*
	 * We break out the loop above on need_resched() and when the
	 * owner changed, which is a sign for heavy contention. Return
	 * success only when lock->owner is NULL.
	 */

/* IAMROOT-12AB:
 * ------------
 * lock owner가 unlock될 때 이미 lock->owner==null이 된다. 그런데
 * 이 루틴에서 또 lock->onwer = null을 한 이유?
 *      - 리스케쥴 요청이 있는 경우에도 lock->onwer = null을 강제 대입하여
 *        heavy contention을 막기 위한 것처럼 주석에서 말하고 있다. 하지만
 *	  아래의 문장은 2015년 2-4월에 걸쳐 코드가 리팩토링 되면서 삭제되었다.
 *     	  (mutex_spin_on_onwer() + owner_running()가 합쳐짐)
 */
	return lock->owner == NULL;
}

/*
 * Initial check for entering the mutex spinning loop
 */
static inline int mutex_can_spin_on_owner(struct mutex *lock)
{
	struct task_struct *owner;
	int retval = 1;

/* IAMROOT-12AB:
 * ------------
 * midpath 조건에 들려면 리스케쥴 요청이 없어야한다.
 * 따라서 요청이 있는 경우 실패
 */
	if (need_resched())
		return 0;

/* IAMROOT-12AB:
 * ------------
 * 이 뮤텍스 락의 owner(task_struct *)가 running 중인지 확인한다.
 * 1=running, 0=not running 
 */
	rcu_read_lock();
	owner = ACCESS_ONCE(lock->owner);
	if (owner)
		retval = owner->on_cpu;
	rcu_read_unlock();
	/*
	 * if lock->owner is not set, the mutex owner may have just acquired
	 * it and not set the owner yet or the mutex has been released.
	 */
	return retval;
}

/*
 * Atomically try to take the lock when it is available
 */
static inline bool mutex_try_to_acquire(struct mutex *lock)
{
/* IAMROOT-12AB:
 * -------------
 * lock->count가 1(unlock) 이면 0(lock)으로 설정한다.
 */
	return !mutex_is_locked(lock) &&
		(atomic_cmpxchg(&lock->count, 1, 0) == 1);
}

/*
 * Optimistic spinning.
 *
 * We try to spin for acquisition when we find that the lock owner
 * is currently running on a (different) CPU and while we don't
 * need to reschedule. The rationale is that if the lock owner is
 * running, it is likely to release the lock soon.
 *
 * Since this needs the lock owner, and this mutex implementation
 * doesn't track the owner atomically in the lock field, we need to
 * track it non-atomically.
 *
 * We can't do this for DEBUG_MUTEXES because that relies on wait_lock
 * to serialize everything.
 *
 * The mutex spinners are queued up using MCS lock so that only one
 * spinner can compete for the mutex. However, if mutex spinning isn't
 * going to happen, there is no point in going through the lock/unlock
 * overhead.
 *
 * Returns true when the lock was taken, otherwise false, indicating
 * that we need to jump to the slowpath and sleep.
 */
static bool mutex_optimistic_spin(struct mutex *lock,
				  struct ww_acquire_ctx *ww_ctx, const bool use_ww_ctx)
{
	struct task_struct *task = current;

/* IAMROOT-12AB:
 * ------------
 * 리스케쥴 요청이 있거나 락의 owner 태스크가 running 중이 아니면 실패(done)
 */
	if (!mutex_can_spin_on_owner(lock))
		goto done;

	/*
	 * In order to avoid a stampede of mutex spinners trying to
	 * acquire the mutex all at once, the spinners need to take a
	 * MCS (queued) lock first before spinning on the owner field.
	 */

/* IAMROOT-12A:
 * ------------
 * 동시에 midpath에 진입하는 cpu를 serialize한다. 즉 먼저 OSQ(MCS) lock을 획득하는
 * cpu 만이 성공리에 리턴된다. OSQ(MCS) lock은 FIFO queue 방식으로 운영되어
 * 인입된 순서대로 공정하게 OSQ(MCS) lock을 획득하게 된다.
 *
 * 대기 queue에 있는 cpu들은 OSQ(MCS) lock을 획득할 때까지 spin 하며 기다리는데
 * 기다리다가 lock->owner cpu가 sleep(preempt)되거나 리스케쥴 요청이 있는 경우는
 * 실패로 리턴되어 goto done으로 이동한다.
 *
 * 물론 MCS lock을 획득한 cpu가 당장 mutex lock을 얻게되는 것이 아니라
 * 이미 mutex lock을 가진 owner가 mutex unlock될 때 까지 spin된 후에 얻게될 것이다.
 *
 * 이와 같이 midpath는 2 단계의 spin 과정을 통과하면 mutex lock을 획득하게 된다.
 *    1) FIFO로 동작하는 OSQ(MCS) lock에서 선두가 될 때까지 osq_lock() 내부에서 spin
 *       - midpath에 혼자 진입한 경우는 이미 선두이므로 osq_lock()내부에서 spin하지 않음.
 *    2) mutex를 소유한 lock->owner가 release될 때까지 아래 while 문에서 spin
 */
	if (!osq_lock(&lock->osq))
		goto done;

/* IAMROOT-12A:
 * ------------
 * 이 while 문장부터 mutex를 획득하려고 spin 한다.
 * OSQ(MCS) lock을 획득한 즉, OSQ의 선두에 있는 cpu만 아래 while 루프에서 spin.
 */
	while (true) {
		struct task_struct *owner;

/* IAMROOT-12AB:
 * ------------
 * use_ww_ctx: W/W mutex에서 사용하는 루틴이므로 추후 분석
 */
		if (use_ww_ctx && ww_ctx->acquired > 0) {
			struct ww_mutex *ww;

			ww = container_of(lock, struct ww_mutex, base);
			/*
			 * If ww->ctx is set the contents are undefined, only
			 * by acquiring wait_lock there is a guarantee that
			 * they are not invalid when reading.
			 *
			 * As such, when deadlock detection needs to be
			 * performed the optimistic spinning cannot be done.
			 */
			if (ACCESS_ONCE(ww->ctx))
				break;
		}

		/*
		 * If there's an owner, wait for it to either
		 * release the lock or go to sleep.
		 */

/* IAMROOT-12AB:
 * ------------
 * 루프를 반복하면서 onwer 값을 읽어온다. owner 값에 따라
 *    - null인 경우 mutex 획득을 시도한다. (아래 mutex_try_to_acquire() 호출)
 *   -  null이 아닌 경우 mutex_spin_on_owner() 함수를 호출하고
 *      리스케쥴 요청으로 인해 함수를 빠져나온 경우(false) break되어 
 *      midpath 실패로 이동한다.
 */
		owner = ACCESS_ONCE(lock->owner);
		if (owner && !mutex_spin_on_owner(lock, owner))
			break;

		/* Try to acquire the mutex if it is unlocked. */
		if (mutex_try_to_acquire(lock)) {
			lock_acquired(&lock->dep_map, ip);

			if (use_ww_ctx) {
				struct ww_mutex *ww;
				ww = container_of(lock, struct ww_mutex, base);

				ww_mutex_set_context_fastpath(ww, ww_ctx);
			}

/* IAMROOT-12AB:
 * -------------
 * 드디어 mutex를 획득했으므로 owner가 되었음을 기록한다.
 */
			mutex_set_owner(lock);
			osq_unlock(&lock->osq);
			return true;
		}

		/*
		 * When there's no owner, we might have preempted between the
		 * owner acquiring the lock and setting the owner field. If
		 * we're an RT task that will live-lock because we won't let
		 * the owner complete.
		 */
		if (!owner && (need_resched() || rt_task(task)))
			break;

		/*
		 * The cpu_relax() call is a compiler barrier which forces
		 * everything in this loop to be re-loaded. We don't need
		 * memory barriers as we'll eventually observe the right
		 * values at the cost of a few extra spins.
		 */
		cpu_relax_lowlatency();
	}

	osq_unlock(&lock->osq);
done:
	/*
	 * If we fell out of the spin path because of need_resched(),
	 * reschedule now, before we try-lock the mutex. This avoids getting
	 * scheduled out right after we obtained the mutex.
	 */
	if (need_resched()) {
		/*
		 * We _should_ have TASK_RUNNING here, but just in case
		 * we do not, make it so, otherwise we might get stuck.
		 */
		__set_current_state(TASK_RUNNING);
		schedule_preempt_disabled();
	}

	return false;
}
#else
static bool mutex_optimistic_spin(struct mutex *lock,
				  struct ww_acquire_ctx *ww_ctx, const bool use_ww_ctx)
{
	return false;
}
#endif

__visible __used noinline
void __sched __mutex_unlock_slowpath(atomic_t *lock_count);

/**
 * mutex_unlock - release the mutex
 * @lock: the mutex to be released
 *
 * Unlock a mutex that has been locked by this task previously.
 *
 * This function must not be used in interrupt context. Unlocking
 * of a not locked mutex is not allowed.
 *
 * This function is similar to (but not equivalent to) up().
 */
void __sched mutex_unlock(struct mutex *lock)
{
	/*
	 * The unlocking fastpath is the 0->1 transition from 'locked'
	 * into 'unlocked' state:
	 */
#ifndef CONFIG_DEBUG_MUTEXES
	/*
	 * When debugging is enabled we must not clear the owner before time,
	 * the slow path will always be taken, and that clears the owner field
	 * after verifying that it was indeed current.
	 */

/* IAMROOT-12AB:
 * -------------
 * lock owner 제거
 */
	mutex_clear_owner(lock);
#endif
	__mutex_fastpath_unlock(&lock->count, __mutex_unlock_slowpath);
}

EXPORT_SYMBOL(mutex_unlock);

/**
 * ww_mutex_unlock - release the w/w mutex
 * @lock: the mutex to be released
 *
 * Unlock a mutex that has been locked by this task previously with any of the
 * ww_mutex_lock* functions (with or without an acquire context). It is
 * forbidden to release the locks after releasing the acquire context.
 *
 * This function must not be used in interrupt context. Unlocking
 * of a unlocked mutex is not allowed.
 */
void __sched ww_mutex_unlock(struct ww_mutex *lock)
{
	/*
	 * The unlocking fastpath is the 0->1 transition from 'locked'
	 * into 'unlocked' state:
	 */
	if (lock->ctx) {
#ifdef CONFIG_DEBUG_MUTEXES
		DEBUG_LOCKS_WARN_ON(!lock->ctx->acquired);
#endif
		if (lock->ctx->acquired > 0)
			lock->ctx->acquired--;
		lock->ctx = NULL;
	}

#ifndef CONFIG_DEBUG_MUTEXES
	/*
	 * When debugging is enabled we must not clear the owner before time,
	 * the slow path will always be taken, and that clears the owner field
	 * after verifying that it was indeed current.
	 */
	mutex_clear_owner(&lock->base);
#endif
	__mutex_fastpath_unlock(&lock->base.count, __mutex_unlock_slowpath);
}
EXPORT_SYMBOL(ww_mutex_unlock);

static inline int __sched
__ww_mutex_lock_check_stamp(struct mutex *lock, struct ww_acquire_ctx *ctx)
{
	struct ww_mutex *ww = container_of(lock, struct ww_mutex, base);
	struct ww_acquire_ctx *hold_ctx = ACCESS_ONCE(ww->ctx);

	if (!hold_ctx)
		return 0;

	if (unlikely(ctx == hold_ctx))
		return -EALREADY;

	if (ctx->stamp - hold_ctx->stamp <= LONG_MAX &&
	    (ctx->stamp != hold_ctx->stamp || ctx > hold_ctx)) {
#ifdef CONFIG_DEBUG_MUTEXES
		DEBUG_LOCKS_WARN_ON(ctx->contending_lock);
		ctx->contending_lock = ww;
#endif
		return -EDEADLK;
	}

	return 0;
}

/*
 * Lock a mutex (possibly interruptible), slowpath:
 */
static __always_inline int __sched
__mutex_lock_common(struct mutex *lock, long state, unsigned int subclass,
		    struct lockdep_map *nest_lock, unsigned long ip,
		    struct ww_acquire_ctx *ww_ctx, const bool use_ww_ctx)
{
	struct task_struct *task = current;
	struct mutex_waiter waiter;
	unsigned long flags;
	int ret;

	preempt_disable();
	mutex_acquire_nest(&lock->dep_map, subclass, 0, nest_lock, ip);

/* IAMROOT-12AB:
 * ------------
 * __mutex_lock_slowpath() 함수에서는 use_ww_ctl=0으로하여 ww mutex를 
 * 동작시키지 않는다.
 */
	if (mutex_optimistic_spin(lock, ww_ctx, use_ww_ctx)) {
		/* got the lock, yay! */
		preempt_enable();
		return 0;
	}

/* IAMROOT-12AB:
 * -------------
 * 동시에 slowpath에 진입한 태스크들에 spin lock을 수행하여 
 * wait_list 등의 연결리스트를 보호한다.
 */
	spin_lock_mutex(&lock->wait_lock, flags);

	/*
	 * Once more, try to acquire the lock. Only try-lock the mutex if
	 * it is unlocked to reduce unnecessary xchg() operations.
	 */

/* IAMROOT-12AB:
 * -------------
 * 한 번 더 lock 상태를 보고 아무도 mutex를 소유하지 않았으면 여기서 획득하고
 * 나갈 수 있는 기회가 있다.
 */
	if (!mutex_is_locked(lock) && (atomic_xchg(&lock->count, 0) == 1))
		goto skip_wait;

	debug_mutex_lock_common(lock, &waiter);
	debug_mutex_add_waiter(lock, &waiter, task_thread_info(task));

	/* add waiting tasks to the end of the waitqueue (FIFO): */

/* IAMROOT-12AB:
 * -------------
 * lock->wait_list 에 waiter(task 정보)를 추가한다.
 */
	list_add_tail(&waiter.list, &lock->wait_list);
	waiter.task = task;

	lock_contended(&lock->dep_map, ip);

	for (;;) {
		/*
		 * Lets try to take the lock again - this is needed even if
		 * we get here for the first time (shortly after failing to
		 * acquire the lock), to make sure that we get a wakeup once
		 * it's unlocked. Later on, if we sleep, this is the
		 * operation that gives us the lock. We xchg it to -1, so
		 * that when we release the lock, we properly wake up the
		 * other waiters. We only attempt the xchg if the count is
		 * non-negative in order to avoid unnecessary xchg operations:
		 */

/* IAMROOT-12AB:
 * -------------
 * 1(unlock)/0(lock)인 경우 -1로 변경(대기자 1명) 그리고 변경전에
 * count가 1(unlocked) 상태였었으면 lock을 획득한 것으로 간주하고 break
 */
		if (atomic_read(&lock->count) >= 0 &&
		    (atomic_xchg(&lock->count, -1) == 1))
			break;

		/*
		 * got a signal? (This code gets eliminated in the
		 * TASK_UNINTERRUPTIBLE case.)
		 */

/* IAMROOT-12AB:
 * -------------
 * 일반 mutex_lock()을 호출하였을 때 TASK_UNINTERRUPTIBLE로 호출되는데
 * 이런 경우 signal이 pending될게 없으므로 이루틴이 동작하지 않는다.
 * 인수가 TASK_INTERRUPTIBLE로 호출된 경우에는 mutex 획득을 중단할 수도 있다.
 */
		if (unlikely(signal_pending_state(state, task))) {
			ret = -EINTR;
			goto err;
		}


/* IAMROOT-12AB:
 * -------------
 * ww-mutex에서 사용하는 루틴.(dead-lock avoidance)
 */
		if (use_ww_ctx && ww_ctx->acquired > 0) {
			ret = __ww_mutex_lock_check_stamp(lock, ww_ctx);
			if (ret)
				goto err;
		}

/* IAMROOT-12AB:
 * -------------
 * TODO: 상태를 왜 다시 저장하는지 확인 필요
 * 추측: mutex_lock_interruptible_nested()등이 아닌 mutex_lock() 함수 호출 시
 *       TASK_UNINTERRUPTIBLE로 설정 후 schedule() 호출 시 스스로 깨어나지 않도록
 *       즉, mutex_unlock()에서 선두의 대기 태스크에대해 wake-up 호출할 때만 
 *       깨어나도록 하였을 것이라 판단된다.
 */
		__set_task_state(task, state);

		/* didn't get the lock, go to sleep: */
		spin_unlock_mutex(&lock->wait_lock, flags);

/* IAMROOT-12AB:
 * -------------
 * 여기서 sleep 한다.
 */
		schedule_preempt_disabled();
		spin_lock_mutex(&lock->wait_lock, flags);
	}

/* IAMROOT-12AB:
 * -------------
 * slowpath에서 sleep 한 후 깨어나 lock을 얻을 때까지 문제없이 잘 동작하려면 
 * 명시적으로 태스크를 TASK_RUNNING 상태로 설정해야 한다.
 * 설정하지 않으면 어떤 문제가???
 */
	__set_task_state(task, TASK_RUNNING);

/* IAMROOT-12AB:
 * -------------
 * 현재 waiter를 리스트에서 제거
 */
	mutex_remove_waiter(lock, &waiter, current_thread_info());
	/* set it to 0 if there are no waiters left: */

/* IAMROOT-12AB:
 * -------------
 * 대기자가 없으면 count에 0(lock)을 대입
 */
	if (likely(list_empty(&lock->wait_list)))
		atomic_set(&lock->count, 0);
	debug_mutex_free_waiter(&waiter);

skip_wait:
	/* got the lock - cleanup and rejoice! */
	lock_acquired(&lock->dep_map, ip);

/* IAMROOT-12AB:
 * -------------
 * mutex를 얻어내었기 때문에 자신이 lock owner인것을 설정한다.
 */
	mutex_set_owner(lock);

	if (use_ww_ctx) {
		struct ww_mutex *ww = container_of(lock, struct ww_mutex, base);
		ww_mutex_set_context_slowpath(ww, ww_ctx);
	}

	spin_unlock_mutex(&lock->wait_lock, flags);
	preempt_enable();
	return 0;

err:
	mutex_remove_waiter(lock, &waiter, task_thread_info(task));
	spin_unlock_mutex(&lock->wait_lock, flags);
	debug_mutex_free_waiter(&waiter);
	mutex_release(&lock->dep_map, 1, ip);
	preempt_enable();
	return ret;
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC
void __sched
mutex_lock_nested(struct mutex *lock, unsigned int subclass)
{
	might_sleep();
	__mutex_lock_common(lock, TASK_UNINTERRUPTIBLE,
			    subclass, NULL, _RET_IP_, NULL, 0);
}

EXPORT_SYMBOL_GPL(mutex_lock_nested);

void __sched
_mutex_lock_nest_lock(struct mutex *lock, struct lockdep_map *nest)
{
	might_sleep();
	__mutex_lock_common(lock, TASK_UNINTERRUPTIBLE,
			    0, nest, _RET_IP_, NULL, 0);
}

EXPORT_SYMBOL_GPL(_mutex_lock_nest_lock);

int __sched
mutex_lock_killable_nested(struct mutex *lock, unsigned int subclass)
{
	might_sleep();
	return __mutex_lock_common(lock, TASK_KILLABLE,
				   subclass, NULL, _RET_IP_, NULL, 0);
}
EXPORT_SYMBOL_GPL(mutex_lock_killable_nested);

int __sched
mutex_lock_interruptible_nested(struct mutex *lock, unsigned int subclass)
{
	might_sleep();
	return __mutex_lock_common(lock, TASK_INTERRUPTIBLE,
				   subclass, NULL, _RET_IP_, NULL, 0);
}

EXPORT_SYMBOL_GPL(mutex_lock_interruptible_nested);

static inline int
ww_mutex_deadlock_injection(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
#ifdef CONFIG_DEBUG_WW_MUTEX_SLOWPATH
	unsigned tmp;

	if (ctx->deadlock_inject_countdown-- == 0) {
		tmp = ctx->deadlock_inject_interval;
		if (tmp > UINT_MAX/4)
			tmp = UINT_MAX;
		else
			tmp = tmp*2 + tmp + tmp/2;

		ctx->deadlock_inject_interval = tmp;
		ctx->deadlock_inject_countdown = tmp;
		ctx->contending_lock = lock;

		ww_mutex_unlock(lock);

		return -EDEADLK;
	}
#endif

	return 0;
}

int __sched
__ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	int ret;

	might_sleep();
	ret =  __mutex_lock_common(&lock->base, TASK_UNINTERRUPTIBLE,
				   0, &ctx->dep_map, _RET_IP_, ctx, 1);
	if (!ret && ctx->acquired > 1)
		return ww_mutex_deadlock_injection(lock, ctx);

	return ret;
}
EXPORT_SYMBOL_GPL(__ww_mutex_lock);

int __sched
__ww_mutex_lock_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	int ret;

	might_sleep();
	ret = __mutex_lock_common(&lock->base, TASK_INTERRUPTIBLE,
				  0, &ctx->dep_map, _RET_IP_, ctx, 1);

	if (!ret && ctx->acquired > 1)
		return ww_mutex_deadlock_injection(lock, ctx);

	return ret;
}
EXPORT_SYMBOL_GPL(__ww_mutex_lock_interruptible);

#endif

/*
 * Release the lock, slowpath:
 */
static inline void
__mutex_unlock_common_slowpath(struct mutex *lock, int nested)
{
	unsigned long flags;

	/*
	 * As a performance measurement, release the lock before doing other
	 * wakeup related duties to follow. This allows other tasks to acquire
	 * the lock sooner, while still handling cleanups in past unlock calls.
	 * This can be done as we do not enforce strict equivalence between the
	 * mutex counter and wait_list.
	 *
	 *
	 * Some architectures leave the lock unlocked in the fastpath failure
	 * case, others need to leave it locked. In the later case we have to
	 * unlock it here - as the lock counter is currently 0 or negative.
	 */

/* IAMROOT-12AB:
 * -------------
 * ARM: __mutex_slowpath_needs_to_unlock() = 1
 * midpath 등이 mutex를 얻기 위해서는 count가 1(unlock)이 되어야 한다.
 */
	if (__mutex_slowpath_needs_to_unlock())
		atomic_set(&lock->count, 1);

	spin_lock_mutex(&lock->wait_lock, flags);
	mutex_release(&lock->dep_map, nested, _RET_IP_);
	debug_mutex_unlock(lock);

/* IAMROOT-12AB:
 * -------------
 * slowpath 대기자가 있는 경우 첫 대기 태스크를 깨운다.
 */
	if (!list_empty(&lock->wait_list)) {
		/* get the first entry from the wait-list: */
		struct mutex_waiter *waiter =
				list_entry(lock->wait_list.next,
					   struct mutex_waiter, list);

		debug_mutex_wake_waiter(lock, waiter);

		wake_up_process(waiter->task);
	}

	spin_unlock_mutex(&lock->wait_lock, flags);
}

/*
 * Release the lock, slowpath:
 */
__visible void
__mutex_unlock_slowpath(atomic_t *lock_count)
{
	struct mutex *lock = container_of(lock_count, struct mutex, count);

	__mutex_unlock_common_slowpath(lock, 1);
}

#ifndef CONFIG_DEBUG_LOCK_ALLOC
/*
 * Here come the less common (and hence less performance-critical) APIs:
 * mutex_lock_interruptible() and mutex_trylock().
 */
static noinline int __sched
__mutex_lock_killable_slowpath(struct mutex *lock);

static noinline int __sched
__mutex_lock_interruptible_slowpath(struct mutex *lock);

/**
 * mutex_lock_interruptible - acquire the mutex, interruptible
 * @lock: the mutex to be acquired
 *
 * Lock the mutex like mutex_lock(), and return 0 if the mutex has
 * been acquired or sleep until the mutex becomes available. If a
 * signal arrives while waiting for the lock then this function
 * returns -EINTR.
 *
 * This function is similar to (but not equivalent to) down_interruptible().
 */
int __sched mutex_lock_interruptible(struct mutex *lock)
{
	int ret;

	might_sleep();
	ret =  __mutex_fastpath_lock_retval(&lock->count);
	if (likely(!ret)) {
		mutex_set_owner(lock);
		return 0;
	} else
		return __mutex_lock_interruptible_slowpath(lock);
}

EXPORT_SYMBOL(mutex_lock_interruptible);

int __sched mutex_lock_killable(struct mutex *lock)
{
	int ret;

	might_sleep();
	ret = __mutex_fastpath_lock_retval(&lock->count);
	if (likely(!ret)) {
		mutex_set_owner(lock);
		return 0;
	} else
		return __mutex_lock_killable_slowpath(lock);
}
EXPORT_SYMBOL(mutex_lock_killable);


/* IAMROOT-12A:
 * ------------
 * __visible		<-- LTO(Link Time Optimization)를 사용하는 경우에도 
 *                          이 함수나 객체를 심볼로 표현한다.
 *
 * LTO:	한 군데서만 호출되는 함수나 객체를 링크 타임에서 inline화 시킨다.
 *      따라서 함수 같은 경우는 심볼로 export 되지 않는 상태가 된다.
 *
 * __sched: .sched.text 섹션에 코드를 위치하게 한다.
 */

__visible void __sched
__mutex_lock_slowpath(atomic_t *lock_count)
{
/* IAMROOT-12A:
 * ------------
 * fastpath 조건이 되지 않으면 이 루틴으로 온다.
 * lock_count는 mutex 구조체의 count 멤버를 가리키는 주소만을 담고 있는데 
 * container_of()를 사용하면 그 구조체의 주소를 알아올 수 있다.
 *
 *			struct abc {
 *	0x0000_3abc		int a;
 *	0x0000_3ac0		int b;
 *				int c;
 *			} _abc;
 *			결과값은 0x0000_3abc
 *
 * int * bbb = &_abc.b;
 * 0x0000_3abc = container_of(bbb, struct abc, b) = &_abc
 */
	struct mutex *lock = container_of(lock_count, struct mutex, count);

/* IAMROOT-12AB:
 * ------------
 * _RET_IP_: 자신을 호출한 함수의 주소로 디버깅 추적을 위해 얻어온다. 
 *	(unsigned long)__builtin_return_address(0)
 */
	__mutex_lock_common(lock, TASK_UNINTERRUPTIBLE, 0,
			    NULL, _RET_IP_, NULL, 0);
}

static noinline int __sched
__mutex_lock_killable_slowpath(struct mutex *lock)
{
	return __mutex_lock_common(lock, TASK_KILLABLE, 0,
				   NULL, _RET_IP_, NULL, 0);
}

static noinline int __sched
__mutex_lock_interruptible_slowpath(struct mutex *lock)
{
	return __mutex_lock_common(lock, TASK_INTERRUPTIBLE, 0,
				   NULL, _RET_IP_, NULL, 0);
}

static noinline int __sched
__ww_mutex_lock_slowpath(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	return __mutex_lock_common(&lock->base, TASK_UNINTERRUPTIBLE, 0,
				   NULL, _RET_IP_, ctx, 1);
}

static noinline int __sched
__ww_mutex_lock_interruptible_slowpath(struct ww_mutex *lock,
					    struct ww_acquire_ctx *ctx)
{
	return __mutex_lock_common(&lock->base, TASK_INTERRUPTIBLE, 0,
				   NULL, _RET_IP_, ctx, 1);
}

#endif

/*
 * Spinlock based trylock, we take the spinlock and check whether we
 * can get the lock:
 */
static inline int __mutex_trylock_slowpath(atomic_t *lock_count)
{
	struct mutex *lock = container_of(lock_count, struct mutex, count);
	unsigned long flags;
	int prev;

	/* No need to trylock if the mutex is locked. */
	if (mutex_is_locked(lock))
		return 0;

	spin_lock_mutex(&lock->wait_lock, flags);

	prev = atomic_xchg(&lock->count, -1);
	if (likely(prev == 1)) {
		mutex_set_owner(lock);
		mutex_acquire(&lock->dep_map, 0, 1, _RET_IP_);
	}

	/* Set it back to 0 if there are no waiters: */
	if (likely(list_empty(&lock->wait_list)))
		atomic_set(&lock->count, 0);

	spin_unlock_mutex(&lock->wait_lock, flags);

	return prev == 1;
}

/**
 * mutex_trylock - try to acquire the mutex, without waiting
 * @lock: the mutex to be acquired
 *
 * Try to acquire the mutex atomically. Returns 1 if the mutex
 * has been acquired successfully, and 0 on contention.
 *
 * NOTE: this function follows the spin_trylock() convention, so
 * it is negated from the down_trylock() return values! Be careful
 * about this when converting semaphore users to mutexes.
 *
 * This function must not be used in interrupt context. The
 * mutex must be released by the same task that acquired it.
 */
int __sched mutex_trylock(struct mutex *lock)
{
	int ret;

	ret = __mutex_fastpath_trylock(&lock->count, __mutex_trylock_slowpath);
	if (ret)
		mutex_set_owner(lock);

	return ret;
}
EXPORT_SYMBOL(mutex_trylock);

#ifndef CONFIG_DEBUG_LOCK_ALLOC
int __sched
__ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	int ret;

	might_sleep();

	ret = __mutex_fastpath_lock_retval(&lock->base.count);

	if (likely(!ret)) {
		ww_mutex_set_context_fastpath(lock, ctx);
		mutex_set_owner(&lock->base);
	} else
		ret = __ww_mutex_lock_slowpath(lock, ctx);
	return ret;
}
EXPORT_SYMBOL(__ww_mutex_lock);

int __sched
__ww_mutex_lock_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	int ret;

	might_sleep();

	ret = __mutex_fastpath_lock_retval(&lock->base.count);

	if (likely(!ret)) {
		ww_mutex_set_context_fastpath(lock, ctx);
		mutex_set_owner(&lock->base);
	} else
		ret = __ww_mutex_lock_interruptible_slowpath(lock, ctx);
	return ret;
}
EXPORT_SYMBOL(__ww_mutex_lock_interruptible);

#endif

/**
 * atomic_dec_and_mutex_lock - return holding mutex if we dec to 0
 * @cnt: the atomic which we are to dec
 * @lock: the mutex to return holding if we dec to 0
 *
 * return true and hold lock if we dec to 0, return false otherwise
 */
int atomic_dec_and_mutex_lock(atomic_t *cnt, struct mutex *lock)
{
	/* dec if we can't possibly hit 0 */
	if (atomic_add_unless(cnt, -1, 1))
		return 0;
	/* we might hit 0, so take the lock */
	mutex_lock(lock);
	if (!atomic_dec_and_test(cnt)) {
		/* when we actually did the dec, we didn't hit 0 */
		mutex_unlock(lock);
		return 0;
	}
	/* we hit 0, and we hold the lock */
	return 1;
}
EXPORT_SYMBOL(atomic_dec_and_mutex_lock);
