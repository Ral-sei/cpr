#ifndef CPR_ASYNC_H
#define CPR_ASYNC_H

#if defined(PLATFORM_XBOX360)
#include "async_wrapper.h"
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <type_traits>
#include <tuple>
#include <utility>
namespace cpr {

// Xbox 360 XDK does not migrate a newly-created thread automatically: it
// starts on the creator's hardware thread.  The old implementation created
// one thread per request and made that thread spin behind a four-slot gate.
// Apart from the creation/stack cost, this put HTTP, image and API bursts on
// the UI processor.  Keep a small permanent pool instead.  The worker
// processors deliberately use the odd hardware threads; processor 0 is
// normally the title/UI thread and XDK reserves some even threads for system
// services.  This is a scheduling policy, not a requirement of cpr itself.
class XboxAsyncPool {
public:
    static XboxAsyncPool& instance() {
        static XboxAsyncPool pool;
        return pool;
    }

    void submit(std::function<void()> task) {
        ensureStarted();
        EnterCriticalSection(&queueLock_);
        queue_.push_back(std::move(task));
        LeaveCriticalSection(&queueLock_);
        ReleaseSemaphore(workAvailable_, 1, nullptr);
    }

private:
    static constexpr int kWorkerCount = 3;

    XboxAsyncPool()
        : workAvailable_(CreateSemaphore(nullptr, 0, 0x7fffffff, nullptr)), started_(0) {
        InitializeCriticalSection(&queueLock_);
    }

    void ensureStarted() {
        if (InterlockedCompareExchange(&started_, 1, 0) != 0) return;

        // CreateThread documentation recommends creating suspended when a
        // processor must be selected before the first instruction executes.
        static const DWORD processors[kWorkerCount] = {1, 3, 5};
        for (int i = 0; i < kWorkerCount; ++i) {
            // Keep the title's normal 64 KiB stack for curl + mbedTLS.
            // 32 KiB is sufficient for a small queue worker, but TLS
            // certificate parsing and ECP operations have substantially
            // deeper call frames on Xenon.  XDK turns stack overflow into a
            // title fault rather than a recoverable C++ error.
            HANDLE thread = CreateThread(nullptr, 64 * 1024, workerEntry,
                                         this,
                                         CREATE_SUSPENDED, nullptr);
            if (!thread) continue;
            XSetThreadProcessor(thread, processors[i]);
            ResumeThread(thread);
            // Workers live for the title lifetime.  Closing our handle does
            // not terminate them; it only avoids retaining an unused handle.
            XCloseHandle(thread);
        }
    }

    static DWORD WINAPI workerEntry(LPVOID argument) {
        // Do not call instance() here.  The first submit() is also the
        // construction of the function-local singleton; re-entering it from
        // a resumed worker is not supported by the Xenon libc++ guard.
        XboxAsyncPool& pool = *static_cast<XboxAsyncPool*>(argument);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            WaitForSingleObject(pool.workAvailable_, INFINITE);
            std::function<void()> task;
            EnterCriticalSection(&pool.queueLock_);
            if (!pool.queue_.empty()) {
                task = std::move(pool.queue_.front());
                pool.queue_.pop_front();
            }
            LeaveCriticalSection(&pool.queueLock_);
            if (task) task();
        }
    }

    CRITICAL_SECTION queueLock_;
    HANDLE workAvailable_;
    std::deque<std::function<void()>> queue_;
    volatile LONG started_;
};

template <class Fn, class... Args>
auto async(Fn&& fn, Args&&... args) {
    using Result = decltype(fn(std::forward<Args>(args)...));
    using Value = std::decay_t<Result>;
    using Wrapper = AsyncWrapper<Value>;
    auto result = Wrapper{};
    auto worker = std::make_unique<std::function<void()>>(
        [result, fn = std::forward<Fn>(fn),
         args_tuple = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            if constexpr (std::is_void<Value>::value) {
                std::apply(fn, std::move(args_tuple));
            } else {
                result.complete([&]() -> Value { return std::apply(fn, std::move(args_tuple)); });
            }
        });
    std::function<void()> task = [worker = worker.release()] {
        std::unique_ptr<std::function<void()>> owned(worker);
        (*owned)();
    };
    XboxAsyncPool::instance().submit(std::move(task));
    return result;
}
class async {
public:
    static void startup(size_t = 1, size_t = 1, int = 0) {}
    static void cleanup() {}
};
}
#else

#include "async_wrapper.h"
#include "singleton.h"
#include "threadpool.h"

namespace cpr {

class GlobalThreadPool : public ThreadPool {
    CPR_SINGLETON_DECL(GlobalThreadPool)
  protected:
    GlobalThreadPool() = default;

  public:
    ~GlobalThreadPool() override = default;
};

/**
 * Return a wrapper for a future, calling future.get() will wait until the task is done and return RetType.
 * async(fn, args...)
 * async(std::bind(&Class::mem_fn, &obj))
 * async(std::mem_fn(&Class::mem_fn, &obj))
 **/
template <class Fn, class... Args>
auto async(Fn&& fn, Args&&... args) {
    return AsyncWrapper{GlobalThreadPool::GetInstance()->Submit(std::forward<Fn>(fn), std::forward<Args>(args)...)};
}

class async {
  public:
    static void startup(size_t min_threads = CPR_DEFAULT_THREAD_POOL_MIN_THREAD_NUM, size_t max_threads = CPR_DEFAULT_THREAD_POOL_MAX_THREAD_NUM, std::chrono::milliseconds max_idle_ms = CPR_DEFAULT_THREAD_POOL_MAX_IDLE_TIME) {
        GlobalThreadPool* gtp = GlobalThreadPool::GetInstance();
        if (gtp->IsStarted()) {
            return;
        }
        gtp->SetMinThreadNum(min_threads);
        gtp->SetMaxThreadNum(max_threads);
        gtp->SetMaxIdleTime(max_idle_ms);
        gtp->Start();
    }

    static void cleanup() {
        GlobalThreadPool::ExitInstance();
    }
};

} // namespace cpr

#endif

#endif // PLATFORM_XBOX360
