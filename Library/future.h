#pragma once

#include <mutex>
#include <thread>
#include <cstddef>
#include <type_traits>
#include <condition_variable>

#if __has_include(<x86intrin.h>)
#include <x86intrin.h>
#define IDLE _mm_pause()
#elif __has_include(<intrin.h>)
#include <intrin.h>
#define IDLE _mm_pause()
#else
#define IDLE
#endif

#include "task.h"

constexpr int hardware_destructive_interference_size = 64;

enum class future_errc : int {
    broken_promise = 0,
    future_already_retrieved = 1,
    promise_already_satisfied = 2,
    no_state = 3
};

class future_error : public std::logic_error {
public:
    explicit future_error(future_errc ec)
            :logic_error(__get_error_string(ec)), __code(ec) { }

    const auto& code() const noexcept { return __code; }

    [[noreturn]] static void __throw(future_errc ec) { throw future_error(ec); }
private:
    static const char* __get_error_string(future_errc __code) noexcept {
        switch (__code) {
        case future_errc::broken_promise: return "future_error:broken promise";
        case future_errc::future_already_retrieved: return "future_error:future_already_retrieved";
        case future_errc::promise_already_satisfied: return "future_error:promise_already_satisfied";
        case future_errc::no_state: return "future_error:no_state";
        }
    }
    future_errc __code;
};

class __shared_assoc_state_base {
private:
    // Data Store
    std::exception_ptr __excp = nullptr;
    alignas(hardware_destructive_interference_size) mutable std::atomic_int __ref{0};
public:
    void set_exception(std::exception_ptr p) {
        __prepare_write();
        __excp = p;
        __complete_write();
    }

    void set_exception_suppress_check(std::exception_ptr p) {
        __prepare_write_suppress_check();
        __excp = p;
        __complete_write_suppress_check();
    }

    void __acquire() noexcept { __ref.fetch_add(1); }

    void __release() noexcept { if (__ref.fetch_sub(1)==1) delete this; }
private:
    // state control
    mutable std::atomic<uintptr_t> __lock{0};

    enum __lck_bits {
        spin_bit = 0b1,
        write_bit = 0b10,
        ready_bit = 0b100,
        pmr_bit_rev = 0b111
    };

    struct alignas(8) __sync_pmr_data {
        std::mutex __m{};
        std::condition_variable __v{};
    };

    auto __get_pmr_address() const noexcept {
        return reinterpret_cast<__sync_pmr_data*>(__lock.load() & (~uintptr_t(pmr_bit_rev)));
    }

    bool __have_pmr_data() const noexcept { return __get_pmr_address(); }

    auto __enable_pmr() const {
        auto __new_pmr = new __sync_pmr_data();
        __lock.fetch_or(reinterpret_cast<uintptr_t>(__new_pmr));
        return __new_pmr;
    }

    void __notify_if_has_pmr() noexcept {
        if (auto __pmr = __get_pmr_address(); __pmr) {
            std::lock_guard<std::mutex> __lk(__pmr->__m);
            __pmr->__v.notify_all();
        }
    }

    bool __try_acquire_write_access() noexcept {
        size_t __iters = 0;
        for (;;) {
            while (__check_write_bit() && (!__check_ready_bit()))
                if (++__iters<100) IDLE; else std::this_thread::yield();
            if (__check_ready_bit()) return false;
            auto _ = __lock.load();
            if (__lock.compare_exchange_strong(_, _ | write_bit, std::memory_order_acquire)) return true;
        }
    }

    bool __check_spin_lock() const noexcept {
        return static_cast<bool>(__lock.load(std::memory_order_relaxed) & spin_bit);
    }

    void __acquire_spin_lock() const noexcept {
        size_t __iters = 0;
        for (;;) {
            while (__check_spin_lock()) if (++__iters<100) IDLE; else std::this_thread::yield();
            auto _ = __lock.load();
            if (__lock.compare_exchange_strong(_, _ | spin_bit, std::memory_order_acquire)) return;
        }
    }

    void __release_spin_lock() const noexcept { __lock.fetch_and(~uintptr_t(spin_bit)); }

    auto __prepare_wait() const {
        if (auto addr = __get_pmr_address(); addr) return addr;
        __acquire_spin_lock();
        auto pmr = __enable_pmr();
        __release_spin_lock();
        return pmr;
    }
protected:
    void __prepare_write() {
        auto success = __try_acquire_write_access();
        if (!success) future_error::__throw(future_errc::promise_already_satisfied);
    }

    void __complete_write() noexcept {
        __lock.fetch_or(ready_bit);
        __notify_if_has_pmr();
        __fire_task();
    }

    void __prepare_write_suppress_check() noexcept { }

    void __complete_write_suppress_check() noexcept {
        __lock.fetch_or(ready_bit | write_bit);
        __notify_if_has_pmr();
        __fire_task();
    }

    void __cancel_write() noexcept { __lock.fetch_and(~uintptr_t(write_bit)); }
public:
    void wait() const {
        if (!is_ready()) {
            auto _ = __prepare_wait();
            std::unique_lock<std::mutex> __lk(_->__m);
            while (!is_ready())
                _->__v.wait(__lk);
        }
    }

    template <class _Clock, class _Duration>
    bool wait_until(const std::chrono::time_point<_Clock, _Duration>& __abs_time) const {
        if (!is_ready()) {
            auto _ = __prepare_wait();
            std::unique_lock<std::mutex> __lk(_->__m);
            while (!is_ready() && _Clock::now()<__abs_time)
                _->__v.wait_until(__lk, __abs_time);
        }
        return is_ready();
    }

    template <class _Rep, class _Period>
    bool wait_for(const std::chrono::duration<_Rep, _Period>& __rel_time) const {
        return wait_until(std::chrono::steady_clock::now()+__rel_time);
    }

    bool is_ready() const noexcept { return __check_ready_not_expire(); }
private:
    bool __check_write_bit() const noexcept { return static_cast<bool>(__lock.load() & write_bit); }

    bool __check_ready_bit() const noexcept { return static_cast<bool>(__lock.load() & ready_bit); }

    bool __check_ready_not_expire() const noexcept { return __check_ready_bit() && __check_write_bit(); }

    void __make_expire() noexcept { __lock.fetch_and(~uintptr_t(write_bit)); }
protected:
    void __prepare_get() {
        wait();
        __acquire_spin_lock();
        if (!__check_ready_not_expire()) {
            __release_spin_lock();
            future_error::__throw(future_errc::future_already_retrieved);
        }
        __make_expire();
        __release_spin_lock();
        if (__excp)
            std::rethrow_exception(__excp);
    }

    void __revert_expire() noexcept { __lock.fetch_or(write_bit); }
public:
    bool __is_satisfied() const noexcept { return __check_ready_bit(); }
    bool valid() const noexcept { return !(__check_write_bit() && (!__check_write_bit())); }
    // Continable
public:
    struct __task : task {
        bool __call_inplace = true;
    };

    void __set_task(__task* _task) noexcept {
        auto prev = __contiune.exchange(_task);
        if (prev) __fire_task();
    }
private:
    std::atomic<__task*> __contiune{nullptr};

    __task* __get_task() noexcept { return __contiune.exchange(reinterpret_cast<__task*>(uintptr_t(~0))); }

    static void __thread_pool_dispatch_one_with_current_piro(task*) { }

    void __fire_task() noexcept {
        if (auto __task = __get_task(); __task) {
            if (__task->__call_inplace)
                __task->fire();
            else
                __thread_pool_dispatch_one_with_current_piro(__task);
        }
    }
public:
    virtual ~__shared_assoc_state_base() { delete __get_pmr_address(); }
};

template <class T>
class __shared_assoc_state final : public __shared_assoc_state_base {
public:
    ~__shared_assoc_state() { reinterpret_cast<T*>(&__val)->~T(); }

    void set_value(T&& v) {
        __prepare_write();
        if constexpr(std::is_nothrow_move_constructible_v<T>)
            new(&__val) T(std::forward<T>(v));
        else
            try {
                new(&__val) T(std::forward<T>(v));
            }
            catch (...) {
                __cancel_write();
                throw;
            }
        __complete_write();
    }

    void set_value(const T& v) {
        __prepare_write();
        if constexpr(std::is_nothrow_copy_constructible_v<T>)
            new(&__val) T(v);
        else
            try {
                new(&__val) T(v);
            }
            catch (...) {
                __cancel_write();
                throw;
            }
        __complete_write();
    }

    void set_value_suppress_check(T&& v) noexcept(std::is_nothrow_move_constructible_v<T>) {
        __prepare_write_suppress_check();
        new(&__val) T(std::forward<T>(v));
        __complete_write_suppress_check();
    }

    void set_value_suppress_check(const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        __prepare_write_suppress_check();
        new(&__val) T(v);
        __complete_write_suppress_check();
    }

    T get() {
        __prepare_get();
        if constexpr(std::is_nothrow_move_constructible_v<T>)
            return std::move(*reinterpret_cast<T*>(&__val));
        else
            try {
                return *reinterpret_cast<T*>(&__val);
            }
            catch (...) {
                __revert_expire();
                throw;
            }
    }
private:
    std::aligned_storage_t<sizeof(T), alignof(T)> __val;
};

template <class T>
class __shared_assoc_state<T&> final : public __shared_assoc_state_base {
public:
    void set_value(T& v) {
        __prepare_write();
        __val = std::addressof(v);
        __complete_write();
    }

    void set_value_suppress_check(T& v) noexcept {
        __prepare_write_suppress_check();
        __val = std::addressof(v);
        __complete_write_suppress_check();
    }

    T& get() {
        __prepare_get();
        return *__val;
    }
private:
    T* __val;
};

template <>
class __shared_assoc_state<void> final : public __shared_assoc_state_base {
public:
    void set_value() {
        __prepare_write();
        __complete_write();
    }

    void set_value_suppress_check() noexcept {
        __prepare_write_suppress_check();
        __complete_write_suppress_check();
    }

    void get() { __prepare_get(); }
};

template <class T>
class __promise_base;

template <class T>
class future;

template <class T>
class promise;

template <class T>
class __future_base {
protected:
    explicit __future_base(__shared_assoc_state<T>* _) noexcept
            :__st(_) { __st->__acquire(); }
private:
    template <class Func>
    struct __pad final : __shared_assoc_state_base::__task {
        using __result_t = std::result_of_t<std::decay_t<Func>(future<T>)>;
        explicit __pad(Func& fn)
                :__fn(std::move(fn)) { }
        void fire() override;
        void __invoke() noexcept(noexcept(__fn(__st)));
        Func __fn;
        promise<__result_t> __promise;
        future<T> __st;
    };
public:
    __future_base() = default;

    __future_base(__future_base&& other) noexcept
            :__st(other.__st) { other.__st = nullptr; }

    __future_base& operator=(__future_base&& other) noexcept {
        if (this!=std::addressof(other)) {
            __st = other.__st;
            other.__st = nullptr;
        }
        return *this;
    }
    __future_base(const __future_base&) = delete;

    __future_base& operator=(const __future_base&) = delete;

    ~__future_base() { __release_state(); }

    bool valid() const noexcept { return __st; }

    bool is_ready() const noexcept { return __st->is_ready(); }

    void wait() const { __st->wait(); }

    template <class _Clock, class _Duration>
    bool wait_until(const std::chrono::time_point<_Clock, _Duration>& __time) const { return __st->wait_until(__time); }

    template <class _Rep, class _Period>
    bool wait_for(const std::chrono::duration<_Rep, _Period>& __rel_time) const { return __st->wait_until(__rel_time); }

    template <class Func>
    future<typename __pad<Func>::__result_t> then(Func fn) {
        auto __task = new __pad<Func>(fn);
        __task->__st = future(nullptr, __st);
        __st->__set_task(__task);
        __st = nullptr;
        return __task->__promise.get_future();
    }
protected:
    void __release_state() noexcept {
        if (__st) {
            __st->__release();
            __st = nullptr;
        }
    }

    struct __release_guard {
        explicit __release_guard(__future_base* _) noexcept
                :__this(_) { }
        __release_guard(__release_guard&&) = delete;
        __release_guard& operator=(__release_guard&&) = delete;
        ~__release_guard() noexcept { __this->__release_state(); }
        __future_base* __this;
    };

    auto __create_guard() noexcept { return __release_guard(this); }

    __shared_assoc_state<T>* __st = nullptr;
};

template <class T>
class future : public __future_base<T> {
    friend class __promise_base<T>;
    friend class __future_base<T>;
    explicit future(__shared_assoc_state<T>* _) noexcept
            :__future_base<T>(_) { }

    future(std::nullptr_t, __shared_assoc_state<T>* _) noexcept { __future_base<T>::template __st = _; }
public:
    future() noexcept = default;
    auto get() {
        auto _ = __future_base<T>::template __create_guard();
        return __future_base<T>::template __st->get();
    }
};

template <class T>
class future<T&> : public __future_base<T&> {
    friend class __promise_base<T&>;
    friend class __future_base<T&>;
    future<T&> __move_get()&& { return std::move(*this); }
    explicit future(__shared_assoc_state<T&>* _) noexcept
            :__future_base<T&>(_) { }

    future(std::nullptr_t, __shared_assoc_state<T&>* _) noexcept { __future_base<T&>::template __st = _; }
public:
    future() noexcept = default;
    auto& get() {
        auto _ = __future_base<T&>::template __create_guard();
        return __future_base<T&>::template __st->get();
    }
};

template <>
class future<void> : public __future_base<void> {
    friend class __promise_base<void>;
    friend class __future_base<void>;
    explicit future(__shared_assoc_state<void>* _) noexcept
            :__future_base<void>(_) { }

    future(std::nullptr_t, __shared_assoc_state<void>* _) noexcept { __st = _; }
public:
    future() noexcept = default;
    void get() {
        auto _ = __create_guard();
        __st->get();
    }
};

template <class T>
class __promise_base {
public:
    __promise_base() = default;

    __promise_base(__promise_base&& other) noexcept
            :__st(other.__st) { other.__st = nullptr; }

    __promise_base& operator=(__promise_base&& other) noexcept {
        if (this!=std::addressof(other)) {
            __st = other.__st;
            other.__st = nullptr;
        }
        return *this;
    }

    __promise_base(const __promise_base&) = delete;

    __promise_base& operator=(const __promise_base&) = delete;

    ~__promise_base() noexcept {
        if (__st) {
            if (!__st->__is_satisfied())
                set_exception_suppress_check(std::make_exception_ptr(future_error(future_errc::broken_promise)));
            __st->__release();
        }
    }

    future<T> get_future() { return future<T>(__st); }

    void set_exception(std::exception_ptr p) { __st->set_exception(p); };

    void set_exception_suppress_check(std::exception_ptr p) noexcept { __st->set_exception_suppress_check(p); }
protected:
    __shared_assoc_state<T>& __get_state() {
        if (__st) return *__st;
        future_error::__throw(future_errc::no_state);
    }
    __shared_assoc_state<T>* __st = make_assoc_state();
private:
    __shared_assoc_state<T>* make_assoc_state() {
        auto _ = new __shared_assoc_state<T>;
        _->__acquire();
        return _;
    }
};

template <class T>
class promise : public __promise_base<T> {
public:
    void set_value(T&& v) { __promise_base<T>::template __get_state().set_value(std::move(v)); }

    void set_value(const T& v) { __promise_base<T>::template __get_state().set_value(v); }

    void set_value_suppress_check(T&& v) noexcept(std::is_nothrow_move_constructible_v<T>) {
        __promise_base<T>::template __get_state().set_value_suppress_check(std::move(v));
    }

    void set_value_suppress_check(const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        __promise_base<T>::template __get_state().set_value_suppress_check(v);
    }
};

template <class T>
class promise<T&> : public __promise_base<T&> {
public:
    void set_value(T& v) { __promise_base<T&>::template __get_state().set_value(v); }

    void set_value_suppress_check(T& v) noexcept {
        __promise_base<T&>::template __get_state().set_value_suppress_check(v);
    }
};

template <>
class promise<void> : public __promise_base<void> {
public:
    void set_value() { __get_state().set_value(); }

    void set_value_suppress_check() noexcept { __get_state().set_value_suppress_check(); }
};

template <class T>
template <class Func>
void __future_base<T>::__pad<Func>::fire() {
    if constexpr(noexcept(__fn(__st)))
        __invoke();
    else
        try {
            __invoke();
        }
        catch (...) {
            __promise.set_exception_suppress_check(std::current_exception());
        }
    delete this;
}

template <class T>
template <class Func>
void __future_base<T>::__pad<Func>::__invoke() noexcept(noexcept(__fn(__st))) {
    if constexpr (std::is_same_v<__result_t, void>) {
        __fn(this->__st);
        __promise.set_value_suppress_check();
    }
    else
        __promise.set_value_suppress_check(__fn(__st));
}
