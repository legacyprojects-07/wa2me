#pragma once
// wa::function — a small std::function replacement.
//
// Why this exists: Symbian's STL is **STLport 5**, which predates C++11 and has
// no <functional> std::function (nor shared_ptr, unique_ptr, initializer_list,
// <random>…). The client only needs "store a callable, call it later, copy it,
// and tell whether it is empty", so this header provides exactly that for
// 0..3 arguments.
//
// It is deliberately C++03-flavoured: virtual call + clone, one heap object per
// non-empty function, decltype-based SFINAE to reject wrong callables (which
// also keeps the five SignalHook::attach overloads unambiguous when they are
// handed a lambda). It compiles with GCC 4.6 (Symbian GCGE), MinGW and any
// newer compiler, with either STLport or libstdc++.

namespace wa {

namespace detail {

template <typename T> T &declval();

template <bool B, typename T = void> struct enable_if {};
template <typename T> struct enable_if<true, T> { typedef T type; };

// Can `F` be called with these argument types? (SFINAE, no <type_traits>.)
template <typename F> struct callable0 {
    template <typename U> static char probe(decltype(declval<U &>()()) *);
    template <typename U> static long probe(...);
    enum { value = sizeof(probe<F>(0)) == sizeof(char) };
};

template <typename F, typename A1> struct callable1 {
    template <typename U> static char probe(decltype(declval<U &>()(declval<A1>())) *);
    template <typename U> static long probe(...);
    enum { value = sizeof(probe<F>(0)) == sizeof(char) };
};

template <typename F, typename A1, typename A2> struct callable2 {
    template <typename U>
    static char probe(decltype(declval<U &>()(declval<A1>(), declval<A2>())) *);
    template <typename U> static long probe(...);
    enum { value = sizeof(probe<F>(0)) == sizeof(char) };
};

template <typename F, typename A1, typename A2, typename A3> struct callable3 {
    template <typename U>
    static char probe(decltype(declval<U &>()(declval<A1>(), declval<A2>(),
                                             declval<A3>())) *);
    template <typename U> static long probe(...);
    enum { value = sizeof(probe<F>(0)) == sizeof(char) };
};

} // namespace detail

template <typename Signature> class function;

// ── function<R()> ──────────────────────────────────────────────────────────
template <typename R>
class function<R()> {
public:
    function() : impl_(0) {}
    function(const function &other) : impl_(other.impl_ ? other.impl_->clone() : 0) {}
    ~function() { delete impl_; }

    function &operator=(const function &other) {
        if (this != &other) {
            Impl *copy = other.impl_ ? other.impl_->clone() : 0;
            delete impl_;
            impl_ = copy;
        }
        return *this;
    }

    template <typename F>
    function(const F &fn,
             typename detail::enable_if<detail::callable0<F>::value, int>::type = 0)
        : impl_(new Holder<F>(fn)) {}

    bool valid() const { return impl_ != 0; }
    operator bool() const { return impl_ != 0; }
    void clear() { delete impl_; impl_ = 0; }

    R operator()() const { return impl_ ? impl_->call() : R(); }

private:
    struct Impl {
        virtual ~Impl() {}
        virtual R call() = 0;
        virtual Impl *clone() const = 0;
    };
    template <typename F> struct Holder : Impl {
        F fn;
        explicit Holder(const F &f) : fn(f) {}
        R call() { return fn(); }
        Impl *clone() const { return new Holder(fn); }
    };

    Impl *impl_;
};

// ── function<R(A1)> ────────────────────────────────────────────────────────
template <typename R, typename A1>
class function<R(A1)> {
public:
    function() : impl_(0) {}
    function(const function &other) : impl_(other.impl_ ? other.impl_->clone() : 0) {}
    ~function() { delete impl_; }

    function &operator=(const function &other) {
        if (this != &other) {
            Impl *copy = other.impl_ ? other.impl_->clone() : 0;
            delete impl_;
            impl_ = copy;
        }
        return *this;
    }

    template <typename F>
    function(const F &fn,
             typename detail::enable_if<detail::callable1<F, A1>::value, int>::type = 0)
        : impl_(new Holder<F>(fn)) {}

    bool valid() const { return impl_ != 0; }
    operator bool() const { return impl_ != 0; }
    void clear() { delete impl_; impl_ = 0; }

    R operator()(A1 a1) const { return impl_ ? impl_->call(a1) : R(); }

private:
    struct Impl {
        virtual ~Impl() {}
        virtual R call(A1) = 0;
        virtual Impl *clone() const = 0;
    };
    template <typename F> struct Holder : Impl {
        F fn;
        explicit Holder(const F &f) : fn(f) {}
        R call(A1 a1) { return fn(a1); }
        Impl *clone() const { return new Holder(fn); }
    };

    Impl *impl_;
};

// ── function<R(A1, A2)> ────────────────────────────────────────────────────
template <typename R, typename A1, typename A2>
class function<R(A1, A2)> {
public:
    function() : impl_(0) {}
    function(const function &other) : impl_(other.impl_ ? other.impl_->clone() : 0) {}
    ~function() { delete impl_; }

    function &operator=(const function &other) {
        if (this != &other) {
            Impl *copy = other.impl_ ? other.impl_->clone() : 0;
            delete impl_;
            impl_ = copy;
        }
        return *this;
    }

    template <typename F>
    function(const F &fn,
             typename detail::enable_if<detail::callable2<F, A1, A2>::value, int>::type = 0)
        : impl_(new Holder<F>(fn)) {}

    bool valid() const { return impl_ != 0; }
    operator bool() const { return impl_ != 0; }
    void clear() { delete impl_; impl_ = 0; }

    R operator()(A1 a1, A2 a2) const { return impl_ ? impl_->call(a1, a2) : R(); }

private:
    struct Impl {
        virtual ~Impl() {}
        virtual R call(A1, A2) = 0;
        virtual Impl *clone() const = 0;
    };
    template <typename F> struct Holder : Impl {
        F fn;
        explicit Holder(const F &f) : fn(f) {}
        R call(A1 a1, A2 a2) { return fn(a1, a2); }
        Impl *clone() const { return new Holder(fn); }
    };

    Impl *impl_;
};

// ── function<R(A1, A2, A3)> ────────────────────────────────────────────────
template <typename R, typename A1, typename A2, typename A3>
class function<R(A1, A2, A3)> {
public:
    function() : impl_(0) {}
    function(const function &other) : impl_(other.impl_ ? other.impl_->clone() : 0) {}
    ~function() { delete impl_; }

    function &operator=(const function &other) {
        if (this != &other) {
            Impl *copy = other.impl_ ? other.impl_->clone() : 0;
            delete impl_;
            impl_ = copy;
        }
        return *this;
    }

    template <typename F>
    function(const F &fn,
             typename detail::enable_if<
                 detail::callable3<F, A1, A2, A3>::value, int>::type = 0)
        : impl_(new Holder<F>(fn)) {}

    bool valid() const { return impl_ != 0; }
    operator bool() const { return impl_ != 0; }
    void clear() { delete impl_; impl_ = 0; }

    R operator()(A1 a1, A2 a2, A3 a3) const {
        return impl_ ? impl_->call(a1, a2, a3) : R();
    }

private:
    struct Impl {
        virtual ~Impl() {}
        virtual R call(A1, A2, A3) = 0;
        virtual Impl *clone() const = 0;
    };
    template <typename F> struct Holder : Impl {
        F fn;
        explicit Holder(const F &f) : fn(f) {}
        R call(A1 a1, A2 a2, A3 a3) { return fn(a1, a2, a3); }
        Impl *clone() const { return new Holder(fn); }
    };

    Impl *impl_;
};

} // namespace wa
