/* radare - LGPL - Copyright 2009-2022 - pancake, keegan */

#include <r_th.h>

/* locks/mutex/sems */

static bool lock_init(RThreadLock *thl, bool recursive) {
#if HAVE_PTHREAD
	if (recursive) {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init (&attr);
#if !defined(__GLIBC__) || __USE_UNIX98__
		pthread_mutexattr_settype (&attr, PTHREAD_MUTEX_RECURSIVE);
#else
		pthread_mutexattr_settype (&attr, PTHREAD_MUTEX_RECURSIVE_NP);
#endif /* !defined(__GLIBC__) || __USE_UNIX98__ */
		pthread_mutex_init (&thl->lock, &attr);
	} else {
		pthread_mutexattr_t attr;
		pthread_mutexattr_init (&attr);
		pthread_mutex_init (&thl->lock, &attr);
	}
#elif __WINDOWS__
	// TODO: obey `recursive` (currently it is always recursive)
	InitializeCriticalSection (&thl->lock);
#else
#warning Unsupported mutex
	return false;
#endif /* HAVE_PTHREAD */
	return true;
}

static bool r_atomic_exchange(volatile R_ATOMIC_BOOL *data, bool v) {
#if HAVE_STDATOMIC_H
	return atomic_exchange_explicit (data, v, memory_order_acquire);
#elif __GNUC__ && !__TINYC__
	int orig;
	int conv = (int)v;
	__atomic_exchange (data, &conv, &orig, __ATOMIC_ACQUIRE);
	return orig;
#elif _MSC_VER
	int conv = (int)v;
	return InterlockedExchange (data, conv);
#else
	bool orig = *data;
	*data = v;
	return orig;
#endif
}

static void r_atomic_store(volatile R_ATOMIC_BOOL *data, bool v) {
#if HAVE_STDATOMIC_H
	atomic_store_explicit (data, v, memory_order_release);
#elif __GNUC__ && !__TINYC__
	int conv = (int)v;
	__atomic_store (data, &conv, __ATOMIC_RELEASE);
#elif _MSC_VER
	int conv = (int)v;
	while (InterlockedExchange (data, conv) != conv)
		;
#else
	*data = v;
#endif
}

R_API RThreadLock *r_th_lock_new(bool recursive) {
	RThreadLock *thl = R_NEW0 (RThreadLock);
	if (!thl) {
		return NULL;
	}

	if (!lock_init (thl, recursive)) {
		return NULL;
	}

	thl->type = R_TH_LOCK_TYPE_HEAP;
	thl->active = 1;
	thl->activating = 0;
	return thl;
}

R_API int r_th_lock_wait(RThreadLock *thl) {
	r_th_lock_enter (thl); // locks here
	r_th_lock_leave (thl); // releases previous mutex
	return 0;
}

// TODO: return bool
R_API int r_th_lock_enter(RThreadLock *thl) {
	if (!thl) {
		return -1;
	}

	// initialize static locks on acquisition
	if (thl->type == R_TH_LOCK_TYPE_STATIC) {
		// start spinning
		while (r_atomic_exchange (&thl->activating, true))
			;

		if (!thl->active) {
			lock_init (thl, false);
			thl->active = 1;
		}

		// finish spinning
		r_atomic_store (&thl->activating, false);
	}
#if HAVE_PTHREAD
	return pthread_mutex_lock (&thl->lock);
#elif __WINDOWS__
	EnterCriticalSection (&thl->lock);
	return 0;
#else
	return 0;
#endif
}

R_API int r_th_lock_tryenter(RThreadLock *thl) {
	if (!thl) {
		return -1;
	}
#if HAVE_PTHREAD
	return !pthread_mutex_trylock (&thl->lock);
#elif __WINDOWS__
	return TryEnterCriticalSection (&thl->lock);
#else
	return 0;
#endif
}

R_API int r_th_lock_leave(RThreadLock *thl) {
	if (!thl) {
		return -1;
	}
#if HAVE_PTHREAD
	return pthread_mutex_unlock (&thl->lock);
#elif __WINDOWS__
	LeaveCriticalSection (&thl->lock);
	return 0;
#else
	return 0;
#endif
}

R_API void *r_th_lock_free(RThreadLock *thl) {
	if (thl) {
#if HAVE_PTHREAD
		pthread_mutex_destroy (&thl->lock);
#elif __WINDOWS__
		DeleteCriticalSection (&thl->lock);
#endif
		if (thl->type == R_TH_LOCK_TYPE_HEAP) {
			free (thl);
		}
	}
	return NULL;
}
