////////////////////////////////////////////////////////////////////////////////
/// \file threadpool.hpp
/// \brief  Lightweight, fine-grained multitasking through thread pools.
///
///   This header is part of a multi-tasking library that provides low-overhead
/// concurrent scheduling. This is provided through a thread pool, and uses the
/// [work-stealing method](https://en.wikipedia.org/wiki/Work_stealing "Wikipedia: Work stealing")
/// for load balancing.                                                       \n
///   In addition, the library provides a fast scheduling path for tasks spawned
/// by another task within the same pool.                                     \n
///   These avert the majority of scheduling overhead for each new task, which
/// makes fine-grained parallelism feasible.
/// \code
/// #include "threadpool.hpp"
///
/// //    Create a new thread pool, letting the implementation determine the
/// //  number of worker threads to use.
/// ThreadPool pool;
///
/// //  Function pointers of type void (*) () can be passed as tasks directly.
/// void task (void)
/// {
///   //  ...
/// }
/// //    Put a task into the pool. Because this isn't called from within a
/// //  worker thread, the worker threads synchronize to avoid calling it twice.
/// pool.schedule([](void)
/// {
/// //    Put a task into the pool. This is called from within a worker thread,
/// //  so no synchronization is required.
///   pool.schedule(&task);
///
/// //    Put a task into the pool, treated as if it were part of the currently
/// //  running task. This is called from within a worker thread, so no
/// //  synchronization is required.
///   pool.schedule_subtask([](void) { });
///
///  using namespace std::chrono;
/// //  Put a task into the pool, to be executed 2 seconds after it is scheduled.
///  pool.schedule_after(seconds(2),
///  [](void) {
///    do_something();
///  });
///
/// //    Put a task into the pool, to be executed at the specified time.
///   pool.schedule_after(steady_clock::now() + seconds(2),
///   [](void) {
///     do_something();
///   });
/// });
///
/// //    When the thread pool is destroyed, remaining tasks are forgotten.
/// \endcode
/// \note Tasks assigned to the pool from within one of its worker threads will
///   take the fast scheduling path unless the worker already has
///   `get_worker_capacity()` tasks scheduled. Tasks assigned from outside the
///   pool will take the slow path.
/// \warning  If `get_concurrency()` active tasks (or more) simultaneously
///   block, then all inactive tasks in the pool may be blocked. To prevent
///   deadlock, it is recommended that tasks be constructed such that at least
///   one active task makes progress.
/// \note Users may define the macro `THREAD_POOL_FALSE_SHARING_ALIGNMENT` to
///   specify L1 cache line size when compiling `threadpool.cpp`. If it is not
///   specified, the library will attempt to use C++17's
///   `hardware_destructive_interference_size`. If that feature is not supported
///   by the compiler, an implementation-defined default value will be selected.
/// \note Users may specify the capacity of each worker's fixed queue by
///   changing the definition of `kLog2Modulus` in `threadpool.cpp`.
/// \todo Allow tasks to return values, possibly using `std::packaged_task`.
/// \todo Investigate delegates as a replacement for `std::function`:
///   ["The Impossibly Fast C++ Delegates (Fixed)"](https://www.codeproject.com/Articles/1170503/The-Impossibly-Fast-Cplusplus-Delegates-Fixed "The Impossibly Fast C++ Delegates")
/// \author Nathaniel J. McClatchey, PhD
/// \version  2.0
/// \copyright Copyright (c) 2017-2019 Nathaniel J. McClatchey, PhD.          \n
///   [Licensed under the MIT license.](https://github.com/nmcclatchey/ThreadPool/blob/master/LICENSE "MIT License")  \n
///   You should have received a copy of the license with this software.
////////////////////////////////////////////////////////////////////////////////

#ifndef THREAD_POOL_HPP_
#define THREAD_POOL_HPP_

#if !defined(__cplusplus) || (__cplusplus < 201103L)
#error  "The ThreadPool library requires C++11 or higher."
#endif

//    For a unified interface to Callable objects, I considered 3 options:
//  * Delegates (fast, but would need extra library and wouldn't allow return)
//  * std::function (universally available, but doesn't allow return)
//  * std::packaged_task (allows return, but may not be available. Eg. MinGW-w64
//  with Win32 threads).
#include <functional>
//  For std::size_t
#include <cstddef>
//  For timed waiting.
#include <chrono>

/// \brief A high-performance asynchronous task scheduler.
/// \warning  If `get_concurrency()` active tasks (or more) simultaneously
///   block, then all inactive tasks in the pool may be blocked. To prevent
///   deadlock, it is recommended that tasks be constructed such that at least
///   one active task makes progress.
/// \note Has a fast path and a slow path. If called by a worker thread,
///   `schedule(const task_type &)` and `schedule_subtask(tconst task_type &)`
///   take the fast path, placing the task into the worker thread's own queue
///   and bypassing any synchronization. If any scheduling function is called by
///   a thread not in the pool or if the worker's queue is at capacity, the slow
///   path is taken, requiring synchronization of the `ThreadPool`'s central
///   queue.
/// \note If the worker's local queue is full, the slow path is taken. If one
///   compiles `threadpool.cpp` without the macro `NDEBUG` defined, a warning
///   will be printed when an over-full queue is first detected.
//    Implementer's note: The [pointer to implementation idiom](http://en.cppreference.com/w/cpp/language/pimpl "C++ Reference: pImpl idiom")
//  provides no significant disadvantage. It will impose a pointer lookup
//  penalty, but only on the slow path. Moreover, dynamic allocation is required
//  regardless, and all initial allocation is combined into a single allocation.
struct ThreadPool
{
/// \brief  A [Callable](https://en.cppreference.com/w/cpp/named_req/Callable "C++ Reference: Named requirements: Callable")
///   type, taking no arguments and returning void. Used to store tasks for
///   later execution.
/// \note   Will be called at most once, then destroyed.
  using task_type = std::function<void()>;

/// \brief  Initializes a thread pool and starts a collection of worker threads.
/// \param[in]  worker_capacity The maximum number of worker threads that the
///   pool will support.
/// \exception  Throws `std::system_error` if the pool was unable to start at
///   least one thread.
///
///   Creates a thread pool with up to *worker_capacity* worker threads, and
/// attempts to start them. If *worker_capacity == 0*, the number of worker
/// threads is positive, but otherwise implementation-defined.
/// \note Use `get_concurrency()` to detect the number of worker threads that
///   were able to start.
  ThreadPool (unsigned worker_capacity = 0);

/// \brief  Destroys the `ThreadPool`, terminating all of its worker threads.
///
///   Notifies all worker threads that work is to be discontinued, and blocks
/// until they terminate. Though any task that has already been started will be
/// completed, any tasks that are not active when `~ThreadPool()` is called
/// may be forgotten.
/// \warning  Using a worker thread to destroy its own `ThreadPool` results in
///   undefined behavior.
  ~ThreadPool (void);

//  Thread pools cannot be copied or moved.
  ThreadPool (ThreadPool const &) = delete;
  ThreadPool & operator= (ThreadPool const &) = delete;

/// \brief  Schedules a task to be performed asynchronously.
/// \param[in]  task  The task to be performed.
///
///   Schedules a task to be performed asynchronously. The task will be called
/// at most once.
/// \par  Memory order
///   Execution of a task *synchronizes-with* (as in
/// [`std::memory_order`](https://en.cppreference.com/w/cpp/atomic/memory_order "C++ Reference: Memory order")
/// ) the call to `schedule()` that added it to the pool, using a
/// [*Release-Acquire*](https://en.cppreference.com/w/cpp/atomic/memory_order#Release-Acquire_ordering "C++ Reference: Release-Acquire ordering")
/// ordering.
  void schedule (task_type const & task);
/// \overload
  void schedule (task_type && task);

/// \brief  Schedules a task to be run asynchronously after a specified wait
///   duration.
/// \param[in]  rel_time  The duration after which the task is to be run.
/// \param[in]  task  The task to be performed.
///
///   Schedules a task to be performed asynchronously, but only after waiting
/// for a duration of *rel_time*. The task will be called at most once.
/// \par  Memory order
///   Execution of a task *synchronizes-with* (as in
/// [`std::memory_order`](https://en.cppreference.com/w/cpp/atomic/memory_order "C++ Reference: Memory order")
/// ) the call to `schedule_after()` that added it to the pool, using a
/// [*Release-Acquire*](https://en.cppreference.com/w/cpp/atomic/memory_order#Release-Acquire_ordering "C++ Reference: Release-Acquire ordering")
/// ordering.
  template<class Rep, class Period, class Task>
  void schedule_after ( std::chrono::duration<Rep, Period> const & rel_time,
                        Task && task)
  {
    using namespace std;
    sched_impl(chrono::duration_cast<duration>(rel_time), forward<Task>(task));
  }

/// \brief  Schedules a task to be run asynchronously at (or after) a specified
///   point in time.
/// \param[in]  time  The time point after which the task is to be run.
/// \param[in]  task  The task to be performed.
///
///   Schedules a task to be performed asynchronously at a specified time point.
/// The task will be called at most once.
/// \par  Memory order
///   Execution of a task *synchronizes-with* (as in
/// [`std::memory_order`](https://en.cppreference.com/w/cpp/atomic/memory_order "C++ Reference: Memory order")
/// ) the call to `schedule_after()` that added it to the pool, using a
/// [*Release-Acquire*](https://en.cppreference.com/w/cpp/atomic/memory_order#Release-Acquire_ordering "C++ Reference: Release-Acquire ordering")
/// ordering.
  template<class Clock, class Duration, class Task>
  void schedule_after ( std::chrono::time_point<Clock, Duration> const & time,
                        Task && task)
  {
    using namespace std;
    using namespace std::chrono;
    sched_impl(duration_cast<duration>(time-Clock::now()), forward<Task>(task));
  }

/// \brief  Schedules a task to be run asynchronously, but with a hint that the
///   task ought to be considered part of the currently-scheduled task.
/// \param[in]  task  The task to be performed.
/// \see `schedule(const task_type &)`
///
///     Schedules a task to be performed asynchronously, but treats it as if it
///   were part of the currently scheduled task. This gives the task a better
///   chance of being performed soon after scheduling, but relaxes
///   non-starvation guarantees. In particular, if the collective subtasks fail
///   to terminate, then the original task is considered not to have terminated,
///   and later tasks may fail to run.                                        \n
///     The `schedule_subtask()` method may be used to encourage (not force)
///   depth-first execution -- rather than breadth-first execution -- if tasks
///   exhibit significant branching. This can reduce the odds of a local queue
///   overflow (the slow path) and reduce the memory needed for scheduled tasks.
///                                                                           \n
///     The task will be called at most once.
/// \par  Memory order
///   Execution of a task *synchronizes-with* (as in
/// [`std::memory_order`](https://en.cppreference.com/w/cpp/atomic/memory_order "C++ Reference: Memory order")
/// ) the call to `schedule_subtask()` that added it to the pool, using a
/// [*Release-Acquire*](https://en.cppreference.com/w/cpp/atomic/memory_order#Release-Acquire_ordering "C++ Reference: Release-Acquire ordering")
/// ordering.
/// \warning  Because a subtask is considered as part of the task that spawned
///   it, no guarantees of non-starvation are made should the collective
///   subtasks not terminate.
  void schedule_subtask (task_type const & task);
/// \overload
  void schedule_subtask (task_type && task);

/// \brief  Returns the number of threads in the pool.
/// \return Number of threads in the pool.
///
///   Returns the number of threads in the `ThreadPool`. That is, this function
/// returns the number of tasks that can be truly executed concurrently or with
/// preemption.
/// \note If more than `get_concurrency()` tasks block simultaneously, the
///   entire `ThreadPool` is blocked, and no further progress will be made.
  unsigned get_concurrency (void) const noexcept;

/// \brief  Maximum number of tasks that can be efficiently scheduled by a
///   worker thread.
/// \return Returns the number of tasks that a worker thread can retain in local
///   storage.
///
///   To reduce contention, each worker thread keeps its own queue of tasks. The
/// queues are pre-allocated, and of constant size. The `get_worker_capacity()`
/// function returns the number of tasks that each worker can keep in its own
/// queue -- that is, the number of tasks that a worker can have scheduled
/// before contention occurs.                                                 \n
///   If the returned value is large, many tasks may be simultaneously scheduled
/// without taking the slow path, but more memory is required. If it is small,
/// task scheduling is more likely to take the slow path, but less memory is
/// required.                                                                 \n
///   To select the size of the worker queues, edit the variable `kLog2Modulus`
/// in `threadpool.cpp`.
  static std::size_t get_worker_capacity (void) noexcept;

/// \brief  Determines whether the pool is currently idle.
/// \return `true` if the pool is idle, or `false` if not.
///
///   Returns whether the pool is idle. That is, returns `true` if all threads
/// in the pool are simultaneously idling, or `false` if at least one thread is
/// active. If the pool is halted, the returned value is undefined. Calling this
/// from within one of the `ThreadPool`'s tasks necessarily returns `false`.
  bool is_idle (void) const;

/// \{
/// \brief  Suspends execution of tasks in the `ThreadPool`.
///
///   Halts all worker threads, blocking the caller until worker threads have
/// fully halted. If `halt()` is called from within one of the pool's worker
/// threads, the calling thread is halted either until `resume()` is called or
/// until the `ThreadPool` is destroyed, whichever comes first.
/// \see  `resume()`
  void halt (void);

/// \brief  Resumes execution of tasks in the `ThreadPool` after a call to
///   `halt()`, or starts threads that had previously failed to initialize.
///
///   Attempts to start, restart, or resume all worker threads.
/// - If all allocated worker threads are already running, this function has no
/// effect.
/// - If execution is currently halted, or the number of active workers is less
/// than that returned by `get_concurrency()`, attempts to re-start all inactive
/// worker threads.
/// .
///   May start fewer worker threads than the total capacity of the pool.     \n
///   May block the caller until all started worker threads have resumed their
/// tasks.
/// \exception  Throws `std::system_error` if the pool was unable to ensure at
///   least one living thread.
/// \see  `halt()`
  void resume (void);

/// \brief  Returns whether the pool is currently halted.
/// \return Returns `true` if all worker threads are halted, or `false` if not.
///
///   Returns whether the pool is currently halted. Note that this function only
/// begins to return `true` once all tasks have fully halted. Calling it from
/// within one of the `ThreadPool`'s tasks necessarily returns `false`.
  bool is_halted (void) const;
/// \}
 private:
  void * impl_;
  using duration = std::chrono::steady_clock::duration;
  void sched_impl (duration const &, task_type const &);
  void sched_impl (duration const &, task_type && task);
};

#endif // THREAD_POOL_HPP_
