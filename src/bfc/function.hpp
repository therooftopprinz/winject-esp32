#ifndef BFC_FUNCTION_HPP_
#define BFC_FUNCTION_HPP_

#include <cstddef>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>

namespace bfc
{

template <size_t N, typename return_t, typename... args_t>
class function
{
public:
    function() = default;

    function(std::nullptr_t)
    {
        clear();
    }

    function(const function& other)
    {
        if (other)
        {
            other.copier_(static_cast<void*>(object_),
                          static_cast<const void*>(other.object_));
            copy_meta_from(other);
        }
        else
        {
            clear();
        }
    }

    function(function&& other)
    {
        if (other)
        {
            other.mover_(static_cast<void*>(object_),
                         static_cast<void*>(other.object_));
            copy_meta_from(other);
            other.reset();
        }
        else
        {
            clear();
        }
    }

    template <typename callable_t,
              std::enable_if_t<!std::is_same_v<
                  std::remove_reference_t<callable_t>, function>>* = nullptr>
    function(callable_t&& obj)
    {
        set(std::forward<callable_t>(obj));
    }

    function& operator=(const function& other)
    {
        if (this != &other)
        {
            function tmp(other);
            swap(tmp);
        }
        return *this;
    }

    function& operator=(function&& other)
    {
        if (this != &other)
        {
            reset();
            if (other)
            {
                other.mover_(static_cast<void*>(object_),
                             static_cast<void*>(other.object_));
                copy_meta_from(other);
                other.reset();
            }
            else
            {
                clear();
            }
        }
        return *this;
    }

    template <typename callable_t,
              std::enable_if_t<!std::is_same_v<
                  std::remove_reference_t<callable_t>, function>>* = nullptr>
    function& operator=(callable_t&& obj)
    {
        reset();
        set(std::forward<callable_t>(obj));
        return *this;
    }

    function& operator=(std::nullptr_t)
    {
        reset();
        return *this;
    }

    ~function()
    {
        if (fn_ != nullptr)
        {
            destroyer_(object_);
        }
    }

    explicit operator bool() const
    {
        return fn_ != nullptr;
    }

    void reset()
    {
        if (fn_ != nullptr)
        {
            destroyer_(object_);
        }
        clear();
    }

    return_t operator()(args_t... args) const
    {
        if (fn_ != nullptr)
        {
            return fn_(const_cast<void*>(static_cast<const void*>(object_)),
                       std::forward<args_t>(args)...);
        }
        abort();
    }

    void swap(function& other) noexcept
    {
        if (this == &other)
        {
            return;
        }
        using std::swap;
        swap(fn_, other.fn_);
        swap(destroyer_, other.destroyer_);
        swap(copier_, other.copier_);
        swap(mover_, other.mover_);
        for (size_t i = 0; i < N; ++i)
        {
            swap(object_[i], other.object_[i]);
        }
    }

    friend void swap(function& a, function& b) noexcept
    {
        a.swap(b);
    }

private:
    template <typename callable_t,
              std::enable_if_t<!std::is_same_v<
                  std::remove_reference_t<callable_t>, function>>* = nullptr>
    void set(callable_t&& obj)
    {
        using stored_t = std::decay_t<callable_t>;
        static_assert(N >= sizeof(stored_t),
                      "bfc::function storage too small for callable");
        static_assert(
            alignof(std::max_align_t) % alignof(stored_t) == 0,
            "bfc::function storage not properly aligned for callable");

        new (static_cast<void*>(object_))
            stored_t(std::forward<callable_t>(obj));
        destroyer_ = [](void* p)
        {
            static_cast<stored_t*>(p)->~stored_t();
        };
        copier_ = [](void* p, const void* other)
        {
            new (p) stored_t(*static_cast<const stored_t*>(other));
        };
        mover_ = [](void* p, void* other)
        {
            new (p) stored_t(std::move(*static_cast<stored_t*>(other)));
        };
        fn_ = [](void* p, args_t... args) -> return_t
        {
            return (*static_cast<stored_t*>(p))(std::forward<args_t>(args)...);
        };
    }

    void copy_meta_from(const function& other)
    {
        fn_ = other.fn_;
        destroyer_ = other.destroyer_;
        copier_ = other.copier_;
        mover_ = other.mover_;
    }

    void clear()
    {
        fn_ = nullptr;
    }

    alignas(std::max_align_t) std::byte object_[N]{};
    return_t (*fn_)(void*, args_t...) = nullptr;
    void (*destroyer_)(void*) = nullptr;
    void (*copier_)(void*, const void*) = nullptr;
    void (*mover_)(void*, void*) = nullptr;
};

template <size_t N, typename T>
struct function_type_helper;
template <size_t N, typename return_t, typename... args_t>
struct function_type_helper<N, return_t(args_t...)>
{
    using type = function<N, return_t, args_t...>;
};

template <typename function_t>
using ulight_function = typename function_type_helper<8, function_t>::type;
template <typename function_t>
using light_function = typename function_type_helper<24, function_t>::type;
template <typename function_t>
using big_function = typename function_type_helper<32, function_t>::type;

}  // namespace bfc

#endif  // BFC_FUNCTION_HPP_
