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
            other.copier(static_cast<void*>(object),
                          static_cast<const void*>(other.object));
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
            other.mover(static_cast<void*>(object),
                         static_cast<void*>(other.object));
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
                other.mover(static_cast<void*>(object),
                             static_cast<void*>(other.object));
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
        if (fn != nullptr)
        {
            destroyer(object);
        }
    }

    explicit operator bool() const
    {
        return fn != nullptr;
    }

    void reset()
    {
        if (fn != nullptr)
        {
            destroyer(object);
        }
        clear();
    }

    return_t operator()(args_t... args) const
    {
        if (fn != nullptr)
        {
            return fn(const_cast<void*>(static_cast<const void*>(object)),
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
        swap(fn, other.fn);
        swap(destroyer, other.destroyer);
        swap(copier, other.copier);
        swap(mover, other.mover);
        for (size_t i = 0; i < N; ++i)
        {
            swap(object[i], other.object[i]);
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

        new (static_cast<void*>(object))
            stored_t(std::forward<callable_t>(obj));
        destroyer = [](void* p)
        {
            static_cast<stored_t*>(p)->~stored_t();
        };
        copier = [](void* p, const void* other)
        {
            new (p) stored_t(*static_cast<const stored_t*>(other));
        };
        mover = [](void* p, void* other)
        {
            new (p) stored_t(std::move(*static_cast<stored_t*>(other)));
        };
        fn = [](void* p, args_t... args) -> return_t
        {
            return (*static_cast<stored_t*>(p))(std::forward<args_t>(args)...);
        };
    }

    void copy_meta_from(const function& other)
    {
        fn = other.fn;
        destroyer = other.destroyer;
        copier = other.copier;
        mover = other.mover;
    }

    void clear()
    {
        fn = nullptr;
    }

    alignas(std::max_align_t) std::byte object[N]{};
    return_t (*fn)(void*, args_t...) = nullptr;
    void (*destroyer)(void*) = nullptr;
    void (*copier)(void*, const void*) = nullptr;
    void (*mover)(void*, void*) = nullptr;
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
