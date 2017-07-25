// bthread - A M:N threading library to make applications more concurrent.
// Copyright (c) 2012 Baidu.com, Inc. All Rights Reserved

// Author: Ge,Jun (gejun@baidu.com)
// Date: Tue Jul 10 17:40:58 CST 2012

#include <stddef.h>                         // size_t
#include <gflags/gflags.h>
#include "base/macros.h"                    // ARRAY_SIZE
#include "base/scoped_lock.h"               // BAIDU_SCOPED_LOCK
#include "base/fast_rand.h"
#include "bthread/butex.h"                  // butex_*
#include "bthread/sys_futex.h"              // futex_wake_private
#include "bthread/processor.h"              // cpu_relax
#include "bthread/task_control.h"
#include "bthread/task_group.h"
#include "bthread/timer_thread.h"
#include "bthread/errno.h"

namespace bthread {

static const bthread_attr_t BTHREAD_ATTR_TASKGROUP = {
    BTHREAD_STACKTYPE_UNKNOWN, 0, NULL };

DEFINE_bool(show_bthread_creation_in_vars, false, "When this flags is on, The time "
            "from bthread creation to first run will be recorded and shown "
            "in /vars");

static bool pass_show_bthread_creation_in_vars(const char*, bool) {
    return true;
}
const bool ALLOW_UNUSED dummy_show_bthread_creation_in_vars = ::google::RegisterFlagValidator(
    &FLAGS_show_bthread_creation_in_vars, pass_show_bthread_creation_in_vars);

__thread TaskGroup* tls_task_group = NULL;
__thread LocalStorage tls_bls = BTHREAD_LOCAL_STORAGE_INITIALIZER;

// defined in bthread/key.cpp
extern void return_keytable(bthread_keytable_pool_t*, KeyTable*);

// [Hacky] This is a special TLS set by bthread-rpc privately... to save
// overhead of creation keytable, may be removed later.
BAIDU_THREAD_LOCAL void* tls_unique_user_ptr = NULL;

const TaskStatistics EMPTY_STAT = { 0, 0 };

const size_t OFFSET_TABLE[] = {
#include "bthread/offset_inl.list"
};

int TaskGroup::get_attr(bthread_t tid, bthread_attr_t* out) {
    TaskMeta* const m = address_meta(tid);
    if (m != NULL) {
        const uint32_t given_ver = get_version(tid);
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (given_ver == *m->version_butex) {
            *out = m->attr;
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
}

int TaskGroup::stopped(bthread_t tid) {
    TaskMeta* const m = address_meta(tid);
    if (m != NULL) {
        const uint32_t given_ver = get_version(tid);
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (given_ver == *m->version_butex) {
            return (int)m->stop;
        }
    }
    // If the tid does not exist or version does not match, it's intuitive
    // to treat the thread as "stopped".
    return 1;
}

int stop_and_consume_butex_waiter(
    bthread_t tid, ButexWaiter** pw) {
    TaskMeta* const m = TaskGroup::address_meta(tid);
    if (m != NULL) {
        const uint32_t given_ver = get_version(tid);
        // stopping bthread is not frequent, locking (a spinlock) is acceptable.
        BAIDU_SCOPED_LOCK(m->version_lock);
        // make sense when version matches.
        if (given_ver == *m->version_butex) {  
            m->stop = true;
            // acquire fence guarantees visibility of `interruptible'.
            ButexWaiter* w =
                m->current_waiter.exchange(NULL, base::memory_order_acquire);
            if (w != NULL && !m->interruptible) {
                // Set waiter back if the bthread is not interruptible.
                m->current_waiter.store(w, base::memory_order_relaxed);
                *pw = NULL;
            } else {
                *pw = w;
            }
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
}

// called in butex.cpp
int set_butex_waiter(bthread_t tid, ButexWaiter* w) {
    TaskMeta* const m = TaskGroup::address_meta(tid);
    if (m != NULL) {
        const uint32_t given_ver = get_version(tid);
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (given_ver == *m->version_butex) {
            // Release fence makes m->stop visible to butex_wait
            m->current_waiter.store(w, base::memory_order_release);
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
}

bool TaskGroup::wait_task(bthread_t* tid, size_t* seed, size_t offset) {
    do {
        int rc = _control->wait_task_once(tid, seed, offset);
        if (rc <= 0) {
            return rc == 0;
        }
        if (_rq.volatile_size() != 0) {
            _rq_mutex.lock();
            const bool popped = _rq.pop(tid);
            _rq_mutex.unlock();
            if (popped) {
                return true;
            }
        }
    } while (true);
}

void TaskGroup::run_main_task() {    
    TaskGroup* dummy = this;
    bthread_t tid;
    while (wait_task(&tid, &_steal_seed, _steal_offset)) {
        TaskGroup::sched_to(&dummy, tid);
        DCHECK_EQ(this, dummy);
        DCHECK_EQ(_cur_meta->stack_container, _main_stack_container);
        if (_cur_meta->tid != _main_tid) {
            TaskGroup::task_runner(1/*skip remained*/);
        }
    }
    // stop_main_task() was called.
    // Don't forget to add elapse of last wait_task.
    current_task()->stat.cputime_ns += base::cpuwide_time_ns() - _last_run_ns;
}

TaskGroup::TaskGroup(TaskControl* c)
    :
#ifndef NDEBUG
    _sched_recursive_guard(0),
#endif
    _cur_meta(NULL)
    , _control(c)
    , _num_nosignal(0)
    , _nsignaled(0)
    , _last_run_ns(base::cpuwide_time_ns())
    , _cumulated_cputime_ns(0)
    , _nswitch(0)
    , _last_context_remained(NULL)
    , _last_context_remained_arg(NULL)
    , _main_stack_container(NULL)
    , _main_tid(0)
    , _creation_pthread(pthread_self())
{
    _steal_seed = base::fast_rand();
    _steal_offset = OFFSET_TABLE[_steal_seed % ARRAY_SIZE(OFFSET_TABLE)];
    CHECK(c);
}

TaskGroup::~TaskGroup() {
    if (_main_tid) {
        TaskMeta* m = address_meta(_main_tid);
        CHECK(_main_stack_container == m->stack_container);
        return_stack(m->release_stack());
        return_resource(get_slot(_main_tid));
        _main_tid = 0;
    }
}

int TaskGroup::init(size_t runqueue_capacity) {
    if (_rq.init(runqueue_capacity)) {
        LOG(FATAL) << "Fail to init runqueue";
        return -1;
    }

    StackContainer* sc = get_stack(STACK_TYPE_MAIN, NULL);
    if (NULL == sc) {
        LOG(FATAL) << "Fail to get main stack container";
        return -1;
    }
    base::ResourceId<TaskMeta> slot;
    TaskMeta* m = base::get_resource<TaskMeta>(&slot);
    if (NULL == m) {
        LOG(FATAL) << "Fail to get TaskMeta";
        return -1;
    }
    m->stop = false;
    m->interruptible = true;
    m->about_to_quit = false;
    m->fn = NULL;
    m->arg = NULL;
    // In current implementation, even if we set m->local_storage to empty,
    // everything should be fine because a non-worker pthread never context
    // switches to a bthread, inconsistency between m->local_storage and tls_bls
    // does not result in bug. However to avoid potential future bug,
    // TaskMeta.local_storage is better to be sync with tls_bls otherwise
    // context switching back to this main bthread will restore tls_bls
    // with NULL values which is incorrect.
    m->local_storage = tls_bls;
    m->cpuwide_start_ns = base::cpuwide_time_ns();
    m->stat = EMPTY_STAT;
    m->attr = BTHREAD_ATTR_TASKGROUP;
    m->tid = make_tid(*m->version_butex, slot);
    m->set_stack(sc);

    _cur_meta = m;
    _main_tid = m->tid;
    _main_stack_container = sc;
    _last_run_ns = base::cpuwide_time_ns();
    return 0;
}

void TaskGroup::task_runner(intptr_t skip_remained) {
    // NOTE: tls_task_group is volatile since tasks are moved around
    //       different groups.
    TaskGroup* g = tls_task_group;

    if (!skip_remained) {
        while (g->_last_context_remained) {
            void (*fn)(void*) = g->_last_context_remained;
            g->_last_context_remained = NULL;
            fn(g->_last_context_remained_arg);
            g = tls_task_group;
        }

#ifndef NDEBUG
        --g->_sched_recursive_guard;
#endif
    }

    do {
        // A task can be stopped before it gets running, in which case
        // we may skip user function, but that may confuse user:
        // Most tasks have variables to remember running result of the task,
        // which is often initialized to values indicating success. If an
        // user function is never called, the variables will be unchanged
        // however they'd better reflect failures because the task is stopped
        // abnormally.
        
        // Meta and identifier of the task is persistent in this run.
        TaskMeta* const m = g->_cur_meta;

        if (FLAGS_show_bthread_creation_in_vars) {
            // NOTE: the thread triggering exposure of pending time may spend
            // considerable time because a single bvar::LatencyRecorder
            // contains many bvar.
            g->_control->exposed_pending_time() <<
                (base::cpuwide_time_ns() - m->cpuwide_start_ns) / 1000L;
        }
        
        // Not catch exceptions except ExitException which is for implementing
        // bthread_exit(). User code is intended to crash when an exception is 
        // not caught explicitly. This is consistent with other threading 
        // libraries.
        void* thread_return;
        try {
            thread_return = m->fn(m->arg);
        } catch (ExitException& e) {
            thread_return = e.value();
        } 
        
        // Group is probably changed
        g = tls_task_group;

        // TODO: Save thread_return
        (void)thread_return;

        // Clean tls variables, must be done before changing version_butex
        // otherwise another thread just joined this thread may not see side
        // effects of destructing tls variables.
        KeyTable* kt = m->local_storage.keytable;
        if (kt != NULL) {
            return_keytable(m->attr.keytable_pool, kt);
            // After deletion: tls may be set during deletion.
            m->local_storage.keytable = NULL;
            tls_bls.keytable = NULL;
        }
        
        // Increase the version and wake up all joiners, if resulting version
        // is 0, change it to 1 to make bthread_t never be 0. Any access
        // or join to the bthread after changing version will be rejected.
        // The spinlock is for visibility of TaskGroup::get_attr.
        {
            BAIDU_SCOPED_LOCK(m->version_lock);
            if (0 == ++*m->version_butex) {
                ++*m->version_butex;
            }
        }
        butex_wake_except(m->version_butex, 0);

        // FIXME: the time from quiting fn to here is not counted into cputime
        if (m->attr.flags & BTHREAD_LOG_START_AND_FINISH) {
            LOG(INFO) << "Finished bthread " << m->tid << ", cputime="
                      << m->stat.cputime_ns / 1000000.0 << "ms";
        }

        g->_control->_nbthreads << -1;
        g->set_remained(TaskGroup::_release_last_context, m);
        ending_sched(&g);
        
    } while (g->_cur_meta->tid != g->_main_tid);
    
    // Was called from a pthread and we don't have BTHREAD_STACKTYPE_PTHREAD
    // tasks to run, quit for more tasks.
}

void TaskGroup::_release_last_context(void* arg) {
    TaskMeta* m = (TaskMeta*)arg;
    if (m->stack_type() != STACK_TYPE_PTHREAD) {
        return_stack(m->release_stack()/*may be NULL*/);
    } else {
        // it's _main_stack_container, don't return.
        m->set_stack(NULL);
    }
    return_resource(get_slot(m->tid));
}

int TaskGroup::start_foreground(TaskGroup** pg,
                                bthread_t* __restrict th,
                                const bthread_attr_t* __restrict attr,
                                void * (*fn)(void*),
                                void* __restrict arg) {
    if (__builtin_expect(!fn, 0)) {
        return EINVAL;
    }
    const int64_t start_ns = base::cpuwide_time_ns();
    const bthread_attr_t using_attr = (attr ? *attr : BTHREAD_ATTR_NORMAL);
    base::ResourceId<TaskMeta> slot;
    TaskMeta* m = base::get_resource(&slot);
    if (__builtin_expect(!m, 0)) {
        return ENOMEM;
    }
    CHECK(m->current_waiter.load(base::memory_order_relaxed) == NULL);
    m->stop = false;
    m->interruptible = true;
    m->about_to_quit = false;
    m->fn = fn;
    m->arg = arg;
    CHECK(m->stack_container == NULL);
    m->attr = using_attr;
    m->local_storage = LOCAL_STORAGE_INIT;
    m->cpuwide_start_ns = start_ns;
    m->stat = EMPTY_STAT;
    m->tid = make_tid(*m->version_butex, slot);
    *th = m->tid;
    if (using_attr.flags & BTHREAD_LOG_START_AND_FINISH) {
        LOG(INFO) << "Started bthread " << m->tid;
    }

    TaskGroup* g = *pg;
    g->_control->_nbthreads << 1;
    if (using_attr.flags & BTHREAD_NOSIGNAL) {
        if (g->is_current_pthread_task()) {
            // never create foreground task in pthread.
            g->ready_to_run_nosignal(m->tid);
        } else {
            // NOSIGNAL affects current task, not the new task.
            g->set_remained(ready_to_run_in_worker_nosignal,
                            (void*)g->current_tid());
            TaskGroup::sched_to(pg, m->tid);
        }
        return 0;
    }
    if (g->is_current_pthread_task()) {
        // never create foreground task in pthread.
        g->ready_to_run(m->tid);
    } else {
        g->set_remained(ready_to_run_in_worker, (void*)g->current_tid());
        TaskGroup::sched_to(pg, m->tid);
    }
    return 0;
}

int TaskGroup::start_background(bthread_t* __restrict th,
                                const bthread_attr_t* __restrict attr,
                                void * (*fn)(void*),
                                void* __restrict arg) {
    if (__builtin_expect(!fn, 0)) {
        return EINVAL;
    }
    const int64_t start_ns = base::cpuwide_time_ns();
    const bthread_attr_t using_attr = (attr ? *attr : BTHREAD_ATTR_NORMAL);
    base::ResourceId<TaskMeta> slot;
    TaskMeta* m = base::get_resource(&slot);
    if (__builtin_expect(!m, 0)) {
        return ENOMEM;
    }
    CHECK(m->current_waiter.load(base::memory_order_relaxed) == NULL);
    m->stop = false;
    m->interruptible = true;
    m->about_to_quit = false;
    m->fn = fn;
    m->arg = arg;
    CHECK(m->stack_container == NULL);
    m->attr = using_attr;
    m->local_storage = LOCAL_STORAGE_INIT;
    m->cpuwide_start_ns = start_ns;
    m->stat = EMPTY_STAT;
    m->tid = make_tid(*m->version_butex, slot);
    *th = m->tid;
    if (using_attr.flags & BTHREAD_LOG_START_AND_FINISH) {
        LOG(INFO) << "Started bthread " << m->tid;
    }
    _control->_nbthreads << 1;
    if (using_attr.flags & BTHREAD_NOSIGNAL) {
        ready_to_run_nosignal(m->tid);
        return 0;
    }
    ready_to_run(m->tid);
    return 0;
}

int TaskGroup::join(bthread_t tid, void** return_value) {
    if (__builtin_expect(!tid, 0)) {  // tid of bthread is never 0.
        return EINVAL;
    }
    TaskMeta* m = address_meta(tid);
    if (__builtin_expect(!m, 0)) {  // no bthread used the slot yet.
        return EINVAL;
    }
    int rc = 0;
    const uint32_t expected_version = get_version(tid);
    if (*m->version_butex == expected_version) {
        TaskGroup* g = tls_task_group;
        TaskMeta* caller = NULL;
        if (g != NULL) {
            if (g->current_tid() == tid) {
                // joining self causes indefinite waiting.
                return EINVAL;
            }
            caller = g->current_task();
            caller->interruptible = false;
        }
        rc = butex_wait(m->version_butex, expected_version, NULL);
        if (rc < 0) {
            if (errno == EWOULDBLOCK) {
                // Unmatched version means the thread just terminated.
                rc = 0;
            } else {
                rc = errno;
                CHECK_EQ(ESTOP, rc);
            }
        }
        if (caller) {
            caller->interruptible = true;
        }
    }
    if (return_value) {
        *return_value = NULL;  // TODO: save return value
    }
    return rc;
}

bool TaskGroup::exists(bthread_t tid) {
    if (tid != 0) {  // tid of bthread is never 0.
        TaskMeta* m = address_meta(tid);
        if (m != NULL) {
            return (*m->version_butex == get_version(tid));
        }
    }
    return false;
}

TaskStatistics TaskGroup::main_stat() const {
    TaskMeta* m = address_meta(_main_tid);
    return m ? m->stat : EMPTY_STAT;
}

void TaskGroup::ending_sched(TaskGroup** pg) {
    TaskGroup* g = *pg;
    bthread_t next_tid = 0;
    // Find next task to run, if none, switch to idle thread of the group.
    g->_rq_mutex.lock();
    const bool popped = g->_rq.pop(&next_tid);
    g->_rq_mutex.unlock();
    if (!popped) {
        if (!g->_control->steal_task(
                &next_tid, &g->_steal_seed, g->_steal_offset)) {
            // Jump to main task if there's no task to run.
            next_tid = g->_main_tid;
        }
    }

    TaskMeta* const cur_meta = g->_cur_meta;
    TaskMeta* next_meta = address_meta(next_tid);
    if (next_meta->stack_container == NULL) {
        if (next_meta->stack_type() == cur_meta->stack_type()) {
            // also works with pthread_task scheduling to pthread_task, the
            // transfered stack_container is just _main_stack_container.
            next_meta->set_stack(cur_meta->release_stack());
        } else {
            StackContainer* sc = get_stack(next_meta->stack_type(), task_runner);
            if (sc != NULL) {
                next_meta->set_stack(sc);
            } else {
                // stack_type is BTHREAD_STACKTYPE_PTHREAD or out of memory,
                // In latter case, attr is forced to be BTHREAD_STACKTYPE_PTHREAD.
                // This basically means that if we can't allocate stack, run
                // the task in pthread directly.
                next_meta->attr.stack_type = BTHREAD_STACKTYPE_PTHREAD;
                next_meta->set_stack(g->_main_stack_container);
            }
        }
    }
    sched_to(pg, next_meta);
}

void TaskGroup::sched(TaskGroup** pg) {
    TaskGroup* g = *pg;
    bthread_t next_tid = 0;
    // Find next task to run, if none, switch to idle thread of the group.
    g->_rq_mutex.lock();
    const bool popped = g->_rq.pop(&next_tid);
    g->_rq_mutex.unlock();
    if (!popped) {
        if (!g->_control->steal_task(
                &next_tid, &g->_steal_seed, g->_steal_offset)) {
            // Jump to main task if there's no task to run.
            next_tid = g->_main_tid;
        }
    }
    sched_to(pg, next_tid);
}

void TaskGroup::sched_to(TaskGroup** pg, TaskMeta* next_meta) {
    TaskGroup* g = *pg;
#ifndef NDEBUG
    if ((++g->_sched_recursive_guard) > 1) {
        LOG(FATAL) << "Recursively(" << g->_sched_recursive_guard - 1
                   << ") call sched_to(" << g << ")";
    }
#endif
    // Save errno so that errno is bthread-specific.
    const int saved_errno = errno;
    void* saved_unique_user_ptr = tls_unique_user_ptr;

    TaskMeta* const cur_meta = g->_cur_meta;
    const int64_t now = base::cpuwide_time_ns();
    const int64_t elp_ns = now - g->_last_run_ns;
    g->_last_run_ns = now;
    cur_meta->stat.cputime_ns += elp_ns;
    if (cur_meta->tid != g->main_tid()) {
        g->_cumulated_cputime_ns += elp_ns;
    }
    ++cur_meta->stat.nswitch;
    ++ g->_nswitch;
    // Switch to the task
    if (__builtin_expect(next_meta != cur_meta, 1)) {
        if ((cur_meta->attr.flags & BTHREAD_LOG_CONTEXT_SWITCH) ||
            (next_meta->attr.flags & BTHREAD_LOG_CONTEXT_SWITCH)) {
            LOG(INFO) << "Switch bthread: " << cur_meta->tid << " -> "
                      << next_meta->tid;
        }
        g->_cur_meta = next_meta;
        tls_bls = next_meta->local_storage;
        if (cur_meta->stack_container != NULL) {
            if (next_meta->stack_container != cur_meta->stack_container) {
                bthread_jump_fcontext(&cur_meta->stack_container->context,
                                      next_meta->stack_container->context,
                                      0/*not skip remained*/);
                // probably went to another group, need to assign g again.
                g = tls_task_group;
            }
#ifndef NDEBUG
            else {
                // else pthread_task is switching to another pthread_task, sc
                // can only equal when they're both _main_stack_container
                CHECK(cur_meta->stack_container == g->_main_stack_container);
            }
#endif
        }
        // else because of ending_sched(including pthread_task->pthread_task)
    } else {
        LOG(FATAL) << "bthread=" << g->current_tid() << " sched_to itself!";
    }

    while (g->_last_context_remained) {
        void (*fn)(void*) = g->_last_context_remained;
        g->_last_context_remained = NULL;
        fn(g->_last_context_remained_arg);
        g = tls_task_group;
    }

    // Restore errno
    errno = saved_errno;
    tls_unique_user_ptr = saved_unique_user_ptr;

#ifndef NDEBUG
    --g->_sched_recursive_guard;
#endif
    *pg = g;
}

void TaskGroup::destroy_self() {
    if (_control) {
        _control->_destroy_group(this);
        _control = NULL;
    } else {
        CHECK(false);
    }
}

void TaskGroup::ready_to_run_in_worker(void* arg) {
    return tls_task_group->ready_to_run((bthread_t)arg);
}

void TaskGroup::ready_to_run_in_worker_nosignal(void* arg) {
    return tls_task_group->ready_to_run_nosignal((bthread_t)arg);
}

void TaskGroup::ready_to_run(bthread_t tid) {
    _rq_mutex.lock();
    while (!_rq.push(tid)) {
        // Flush nosignal tasks to avoid the case that the caller start too
        // many no signal threads
        const int val = _num_nosignal;
        _num_nosignal = 0;
        _nsignaled += val;
        _rq_mutex.unlock();
        _control->signal_task(val);

        // A promising approach is to insert the task into another TaskGroup,
        // but we don't use it because:
        // * There're already many bthreads to run, just insert the bthread
        //   into other TaskGroup does not help.
        // * Insertions into other TaskGroups perform worse when all workers
        //   are busy at creating bthreads (proved by test_input_messenger in
        //   baidu-rpc)
        
        // Shall be rare, simply sleep awhile.
        LOG_EVERY_SECOND(ERROR) << "rq is full, capacity=" << _rq.capacity();
        ::usleep(1000);
        _rq_mutex.lock();
    }
    const int additional_signal = _num_nosignal;
    _num_nosignal = 0;
    _nsignaled += 1 + additional_signal;
    _rq_mutex.unlock();
    _control->signal_task(1 + additional_signal);
}

void TaskGroup::ready_to_run_nosignal(bthread_t tid) {
    _rq_mutex.lock();
    while (!_rq.push(tid)) {
        // Flush nosignal tasks to avoid the case that the caller start too
        // many no signal threads
        const int val = _num_nosignal;
        _num_nosignal = 0;
        _nsignaled += val;
        _rq_mutex.unlock();
        _control->signal_task(val);

        // See the comment in ready_to_run()
        LOG_EVERY_SECOND(ERROR) << "rq is full, capacity=" << _rq.capacity();
        ::usleep(1000);
        _rq_mutex.lock();
    }
    ++_num_nosignal;
    _rq_mutex.unlock();
}

int TaskGroup::flush_nosignal_tasks() {
    _rq_mutex.lock();
    const int val = _num_nosignal;
    _num_nosignal = 0;
    _nsignaled += val;
    _rq_mutex.unlock();
    _control->signal_task(val);
    return val;
}

struct SleepArgs {
    uint64_t timeout_us;
    bthread_t tid;
    TaskMeta* meta;
    TaskGroup* group;
};

static void ready_to_run_from_timer_thread(void* arg) {
    CHECK(tls_task_group == NULL);
    const SleepArgs* e = static_cast<const SleepArgs*>(arg);
    e->group->control()->choose_one_group()->ready_to_run(e->tid);
}

void TaskGroup::_add_sleep_event(void* arg) {
    // Must copy SleepArgs. After calling TimerThread::schedule(), previous
    // thread may be stolen by a worker immediately and the on-stack SleepArgs
    // will be gone.
    SleepArgs e = *static_cast<SleepArgs*>(arg);
    TaskGroup* g = e.group;
    
    TimerThread::TaskId sleep_id;
    sleep_id = get_global_timer_thread()->schedule(
        ready_to_run_from_timer_thread, arg,
        base::microseconds_from_now(e.timeout_us));

    if (!sleep_id) {
        // TimerThread is stopping, schedule previous thread.
        // TODO(gejun): Need error?
        g->ready_to_run(e.tid);
        return;
    }
    
    // Set TaskMeta::current_sleep, synchronizing with stop_usleep().
    const uint32_t given_ver = get_version(e.tid);
    {
        BAIDU_SCOPED_LOCK(e.meta->version_lock);
        if (given_ver == *e.meta->version_butex && !e.meta->stop) {
            e.meta->current_sleep = sleep_id;
            return;
        }
    }
    // Fail to set current_sleep when previous thread is stopping or even
    // stopped(unmatched version).
    // Before above code block, stop_usleep() always sees current_sleep == 0.
    // It will not schedule previous thread. The race is between current
    // thread and timer thread.
    if (get_global_timer_thread()->unschedule(sleep_id) == 0) {
        // added to timer, previous thread may be already woken up by timer and
        // even stopped. It's safe to schedule previous thread when unschedule()
        // returns 0 which means "the not-run-yet sleep_id is removed". If the
        // sleep_id is running(returns 1), ready_to_run_in_worker() will
        // schedule previous thread as well. If sleep_id does not exist,
        // previous thread is scheduled by timer thread before and we don't
        // have to do it again.
        g->ready_to_run(e.tid);
    }
}

// To be consistent with sys_usleep, set errno and return -1 on error.
int TaskGroup::usleep(TaskGroup** pg, uint64_t timeout_us) {
    if (0 == timeout_us) {
        int rc = yield(pg);
        if (rc == 0) {
            return 0;
        }
        errno = rc;
        return -1;
    }
    TaskGroup* g = *pg;
    // We have to schedule timer after we switched to next bthread otherwise
    // the timer may wake up(jump to) current still-running context.
    SleepArgs e = { timeout_us, g->current_tid(), g->current_task(), g };
    g->set_remained(_add_sleep_event, &e);
    sched(pg);
    g = *pg;
    e.meta->current_sleep = 0;
    if (e.meta->stop) {
        errno = ESTOP;
        return -1;
    }
    return 0;
}

int TaskGroup::stop_usleep(bthread_t tid) {
    TaskMeta* const m = address_meta(tid);
    if (m == NULL) {
        return EINVAL;
    }
    // Replace current_sleep of the thread with 0 and set stop to true.
    TimerThread::TaskId sleep_id = 0;
    const uint32_t given_ver = get_version(tid);
    {
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (given_ver == *m->version_butex) {
            m->stop = true;
            if (m->interruptible) {
                sleep_id = m->current_sleep;
                m->current_sleep = 0;  // only one stopper gets the sleep_id
            }
        }
    }
    if (sleep_id != 0 && get_global_timer_thread()->unschedule(sleep_id) == 0) {
        ready_to_run(tid);
    }
    return 0;
}

int TaskGroup::yield(TaskGroup** pg) {
    TaskGroup* g = *pg;
    if (!g->current_task()->about_to_quit) {
        g->set_remained(ready_to_run_in_worker, (void*)g->current_tid());
    } else {
        g->set_remained(ready_to_run_in_worker_nosignal,
                        (void*)g->current_tid());
    }
    sched(pg);
    return 0;
}

void print_task(std::ostream& os, bthread_t tid) {
    TaskMeta* const m = TaskGroup::address_meta(tid);
    if (m == NULL) {
        os << "bthread=" << tid << " : never existed";
        return;
    }
    const uint32_t given_ver = get_version(tid);
    bool matched = false;
    bool stop = false;
    bool interruptible = false;
    bool about_to_quit = false;
    void* (*fn)(void*) = NULL;
    void* arg = NULL;
    bthread_attr_t attr = BTHREAD_ATTR_NORMAL;
    bool has_tls = false;
    int64_t cpuwide_start_ns = 0;
    TaskStatistics stat = {0, 0};
    {
        BAIDU_SCOPED_LOCK(m->version_lock);
        if (given_ver == *m->version_butex) {
            matched = true;
            stop = m->stop;
            interruptible = m->interruptible;
            about_to_quit = m->about_to_quit;
            fn = m->fn;
            arg = m->arg;
            attr = m->attr;
            has_tls = m->local_storage.keytable;
            cpuwide_start_ns = m->cpuwide_start_ns;
            stat = m->stat;
        }
    }
    if (!matched) {
        os << "bthread=" << tid << " : not exist now";
    } else {
        os << "bthread=" << tid << " :\nstop=" << stop
           << "\ninterruptible=" << interruptible
           << "\nabout_to_quit=" << about_to_quit
           << "\nfn=" << (void*)fn
           << "\narg=" << (void*)arg
           << "\nattr={stack_type=" << attr.stack_type
           << " flags=" << attr.flags
           << " keytable_pool=" << attr.keytable_pool 
           << "}\nhas_tls=" << has_tls
           << "\nuptime_ns=" << base::cpuwide_time_ns() - cpuwide_start_ns
           << "\ncputime_ns=" << stat.cputime_ns
           << "\nnswitch=" << stat.nswitch;
    }
}

}  // namespace bthread

namespace baidu {
namespace bthread {
void print_task(std::ostream& os, bthread_t tid) {
    ::bthread::print_task(os, tid);
}
extern BAIDU_THREAD_LOCAL void* tls_unique_user_ptr
    __attribute__ ((alias ("_ZN7bthread19tls_unique_user_ptrE")));
}
}
