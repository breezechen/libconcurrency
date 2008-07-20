/*
 * Co-routines in C
 *
 * Possible implementations:
 * 1. stack switching - need to constantly check stack usage (I use this).
 * 2. stack copying - essentially continuations.
 *
 * Notes:
 * * termination of a coroutine without explicit control transfer returns control
 *   to the coroutine which initialized the coro library.
 *
 * Todo:
 * 1. Co-routines must be integrated with any VProc/kernel-thread interface, since
 *    an invoked co-routine might be running on another cpu. A coro invoker must
 *    check that the target vproc is the same as the current vproc; if not, queue the
 *    invoker on the target vproc using an atomic op.
 * 2. VCpu should implement work-stealing, ie. when its run queue is exhausted, it
 *    should contact another VCpu and steal a few of its coros, after checking its
 *    migration queues of course. The rate of stealing should be tuned:
 *    http://www.cs.cmu.edu/~acw/15740/proposal.html
 * 3. Provide an interface to register a coroutine for any errors generated. This is
 *    a type of general Keeper, or exception handling.
 * 4. Optimizations:
 *    a) try to implement an underflow handler to lazily restore parts of the stack.
 *    b) reduce the number of branches to improve branch prediction.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <libconcurrency/coro.h>
#include <libconcurrency/tls.h>
#include "ctxt.h"

/*
 * These are thresholds used to grow and shrink the stack. They are scaled by
 * the size of various platform constants. STACK_TGROW is sized to allow
 * approximately 200 nested calls. On a 32-bit machine assuming 4 words of
 * overhead per call, 256 calls = 1024. If stack allocation is performed,
 * this will need to be increased.
 */
#define STACK_TGROW 1024
#define STACK_DEFAULT sizeof(intptr_t) * STACK_TGROW
#define STACK_TSHRINK 2 * STACK_DEFAULT
#define STACK_ADJ STACK_DEFAULT

/* the coroutine structure */
struct _coro {
	_ctxt ctxt;
	_entry start;
	void * env;
	size_t env_size;
	size_t used;
};
static cvalue cnone = { NULL };

/*
 * Each of these are local to the kernel thread. Volatile storage is necessary
 * otherwise _value is often cached as a local when switching contexts, so
 * a sequence of calls will always return the first value!
 */
THREAD_LOCAL volatile coro _cur;
THREAD_LOCAL volatile cvalue _value;
THREAD_LOCAL struct _coro _on_exit;
THREAD_LOCAL uintptr_t _sp_base;

/*
 * We probe the current machine and extract the data needed to modify the
 * machine context. The current thread is then initialized as the currently
 * executing coroutine.
 */
EXPORT
coro coro_init(void * sp_base)
{
	_infer_stack_direction();
	_sp_base = (intptr_t)sp_base;
	_on_exit.env_size = STACK_DEFAULT;
	_on_exit.env = malloc(_on_exit.env_size);
	_cur = &_on_exit;
	return _cur;
}

void _coro_save(coro to, uintptr_t mark)
{
	uintptr_t sp = (_stack_grows_up ? _sp_base : mark);
	size_t sz = (_stack_grows_up ? mark - _sp_base : _sp_base - mark);
	if (to->env_size < sz + STACK_TGROW || to->env_size > sz - STACK_TSHRINK)
	{
		size_t newsz = sz + STACK_ADJ;
		free(to->env);
		to->env = malloc(newsz);
		to->env_size = newsz;
	}
	to->used = sz;
	memcpy(to->env, (void *)sp, sz);
}

void _coro_restore(uintptr_t target_sz, void ** pad)
{
	if ((uintptr_t)&pad - _sp_base > target_sz) {
		size_t sz = _cur->used;
		void * sp = (void *)(_stack_grows_up ? _sp_base : _sp_base - sz);
		memcpy(sp, _cur->env, sz);
		_rstr_and_jmp(_cur->ctxt);
	} else {
		/* recurse until the stack depth is greater than target stack depth */
		void * padding[STACK_TGROW];
		_coro_restore(target_sz, padding);
	}
}

/*
 * This function invokes the start function of the coroutine when the
 * coroutine is first called. If it was called from coro_new, then it sets
 * up the stack and initializes the saved context.
 */
void _coro_enter(coro c)
{
	if (_save_and_resumed(c->ctxt))
	{	/* start the coroutine; stack is empty at this point. */
		cvalue _return;
		_return.p = _cur;
		_cur->start(_value);
		/* return the exited coroutine to the exit handler */
		coro_call(&_on_exit, _return);
	}
	else
	{
		void * stack_top;
		_coro_save(c, (intptr_t)&stack_top);
	}
}

EXPORT
coro coro_new(_entry fn)
{
	coro c = (coro)malloc(sizeof(struct _coro));
	c->env_size = STACK_DEFAULT;
	c->env = malloc(c->env_size);
	c->start = fn;
	_coro_enter(c);
	return c;
}

/*
 * First, set the value in the volatile global. If _value were not volatile, this value
 * would be cached on the stack, and hence saved and restored on every call. We then
 * save the context for the current coroutine, set the target coroutine as the current
 * one running, then restore it. The target is now running, and it simply returns _value.
 */
EXPORT
cvalue coro_call(coro target, cvalue value)
{
	/* FIXME: ensure target is on the same proc as cur, else, migrate cur to target->proc */
	void * stack_top;
	_value = value; /* pass value to 'target' */
	if (!_save_and_resumed(_cur->ctxt))
	{
		/* _sp_base - &local is used to calculate the size of the env */
		uintptr_t target_sz = (_stack_grows_up
			? target->env_size
			: -target->env_size);
		_coro_save(_cur, (uintptr_t)&stack_top);
		_cur = target;
		_coro_restore(target_sz, NULL);
	}
	/* when someone called us, just return the value */
	return _value;
}

EXPORT
coro coro_clone(coro c)
{
	coro cnew = (coro)malloc(sizeof(struct _coro));
	size_t sz = c->env_size;
	void * env = malloc(sz);
	/* copy the context then the stack data */
	memcpy(cnew, c, sizeof(struct _coro));
	memcpy(env, c->env, sz);
	cnew->env = env;
	cnew->env_size = sz;
	return cnew;
}

EXPORT
void coro_free(coro c)
{
	if (c->env != NULL)
	{
		free((void *)c->env);
	}
	free(c);
}

EXPORT
void coro_poll()
{
	/* mark stack locations for lazy copying */
	ptrdiff_t mark = (_stack_grows_up
		? (intptr_t)&mark - _sp_base
		: _sp_base - (intptr_t)&mark);

	/*
	 * mark a setjmp
	 * extract the ip (or extract the return ip from the stack frame)
	 * create a coroutine using that ip as a "void (*f)(void)" function
	 * coro_call it
	 * this jumps back into the original function, which then returns to _coro_enter when done
	 * _coro_enter would then simply coro_call to the exit_handler, which returns here
	 * possible problems: function prologs, clobbered return values.
	 */
}
