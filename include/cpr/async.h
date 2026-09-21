#ifndef CPR_ASYNC_H
#define CPR_ASYNC_H

#if defined(PLATFORM_XBOX360)
#include "async_wrapper.h"
#include <utility>
namespace cpr {
template <class Fn, class... Args>
auto async(Fn&& fn, Args&&... args) {
    using Result = decltype(fn(std::forward<Args>(args)...));
    if constexpr (std::is_void<Result>::value) {
        fn(std::forward<Args>(args)...);
        return AsyncWrapper<void>{};
    } else {
        return AsyncWrapper<Result>{fn(std::forward<Args>(args)...)};
    }
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
