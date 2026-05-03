#ifndef XYZ_COMPOSABLE_VALUE_TYPES_H
#define XYZ_COMPOSABLE_VALUE_TYPES_H

#include "indirect.h"

#include <atomic>
#include <cassert>
#include <compare>
#include <cstddef>
#include <functional>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>

namespace xyz {

template <typename V>
class copy_on_write;

namespace detail {

template <typename F, typename T>
concept cow_action = std::invocable<F, T&> && std::same_as<std::invoke_result_t<F, T&>, void>;

template <typename F, typename T>
concept cow_transformation =
  std::invocable<F, T const&> && std::same_as<std::invoke_result_t<F, T const&>, T>;

template <typename>
inline constexpr bool is_copy_on_write_v = false;

template <typename V>
inline constexpr bool is_copy_on_write_v<copy_on_write<V>> = true;

template <typename>
inline constexpr bool is_in_place_type_v = false;

template <typename T>
inline constexpr bool is_in_place_type_v<std::in_place_type_t<T>> = true;

} // namespace detail

// copy_on_write<V> adds shared copy-on-write ownership around any owning value
// wrapper V that exposes value_type and allocator_type (e.g. indirect<T, A>).
// V contributes only its type aliases; value_type is stored directly in the
// ref-counted model — there is no extra indirection through V at runtime.
template <typename V>
class copy_on_write
{
  using alloc_traits = std::allocator_traits<typename V::allocator_type>;

  static_assert(std::is_object_v<typename V::value_type>, "V::value_type must be an object type");
  static_assert(!std::is_array_v<typename V::value_type>,
                "V::value_type must not be an array type");
  static_assert(!std::is_same_v<typename V::value_type, std::in_place_t>,
                "V::value_type must not be std::in_place_t");
  static_assert(!detail::is_in_place_type_v<typename V::value_type>,
                "V::value_type must not be a specialization of std::in_place_type_t");
  static_assert(!std::is_const_v<typename V::value_type> &&
                  !std::is_volatile_v<typename V::value_type>,
                "V::value_type must not be cv-qualified");
  static_assert(std::is_same_v<typename V::value_type, typename alloc_traits::value_type>,
                "V::allocator_type::value_type must be V::value_type");

public:
  using value_type = typename V::value_type;
  using allocator_type = typename V::allocator_type;
  using const_pointer = typename alloc_traits::const_pointer;

  //
  // Member functions
  //

  explicit copy_on_write()
    requires std::default_initializable<allocator_type>
    : _alloc{}
    , _self{_make_model(_alloc)}
  {
    static_assert(std::is_default_constructible_v<value_type>);
  }

  template <typename U = value_type>
    requires(!std::same_as<std::remove_cvref_t<U>, copy_on_write> &&
             !std::same_as<std::remove_cvref_t<U>, std::in_place_t> &&
             std::constructible_from<value_type, U> && std::default_initializable<allocator_type>)
  explicit copy_on_write(U&& x)
    : _alloc{}
    , _self{_make_model(_alloc, std::forward<U>(x))}
  {
  }

  template <typename... Us>
    requires(std::constructible_from<value_type, Us...> &&
             std::default_initializable<allocator_type>)
  explicit copy_on_write(std::in_place_t, Us&&... us)
    : _alloc{}
    , _self{_make_model(_alloc, std::forward<Us>(us)...)}
  {
  }

  template <typename I, typename... Us>
    requires(std::constructible_from<value_type, std::initializer_list<I>&, Us...> &&
             std::default_initializable<allocator_type>)
  explicit copy_on_write(std::in_place_t, std::initializer_list<I> ilist, Us&&... us)
    : _alloc{}
    , _self{_make_model(_alloc, ilist, std::forward<Us>(us)...)}
  {
  }

  explicit copy_on_write(std::allocator_arg_t, allocator_type const& a)
    : _alloc{a}
    , _self{_make_model(_alloc)}
  {
    static_assert(std::is_default_constructible_v<value_type>);
  }

  template <typename U = value_type>
    requires(!std::same_as<std::remove_cvref_t<U>, copy_on_write> &&
             !std::same_as<std::remove_cvref_t<U>, std::in_place_t> &&
             std::constructible_from<value_type, U>)
  explicit copy_on_write(std::allocator_arg_t, allocator_type const& a, U&& u)
    : _alloc{a}
    , _self{_make_model(_alloc, std::forward<U>(u))}
  {
  }

  template <typename... Us>
    requires std::constructible_from<value_type, Us...>
  explicit copy_on_write(std::allocator_arg_t,
                         allocator_type const& a,
                         std::in_place_t,
                         Us&&... us)
    : _alloc{a}
    , _self{_make_model(_alloc, std::forward<Us>(us)...)}
  {
  }

  template <typename I, typename... Us>
    requires std::constructible_from<value_type, std::initializer_list<I>&, Us...>
  explicit copy_on_write(std::allocator_arg_t,
                         allocator_type const& a,
                         std::in_place_t,
                         std::initializer_list<I> ilist,
                         Us&&... us)
    : _alloc{a}
    , _self{_make_model(_alloc, ilist, std::forward<Us>(us)...)}
  {
  }

  explicit copy_on_write(std::allocator_arg_t,
                         allocator_type const& a,
                         copy_on_write const& other)
    : _alloc{a}
    , _self{nullptr}
  {
    static_assert(std::is_copy_constructible_v<value_type>);

    if (other.valueless_after_move()) {
      return;
    }

    if (a == other._alloc) {
      _self = other._self;
      _self->count.fetch_add(1, std::memory_order_relaxed);
    } else {
      _self = _make_model(_alloc, *other);
    }
  }

  explicit copy_on_write(std::allocator_arg_t,
                         allocator_type const& a,
                         copy_on_write&& other) noexcept(alloc_traits::is_always_equal::value)
    : _alloc{a}
    , _self{nullptr}
  {
    if (other.valueless_after_move()) {
      return;
    }

    if (alloc_traits::is_always_equal::value || a == other._alloc) {
      _self = std::exchange(other._self, nullptr);
    } else {
      _self = _make_model(_alloc, std::move(other._self->value));
      other._reset(nullptr);
    }
  }

  copy_on_write(copy_on_write const& x)
    : _alloc{alloc_traits::select_on_container_copy_construction(x._alloc)}
    , _self{nullptr}
  {
    static_assert(std::is_copy_constructible_v<value_type>);

    if (x.valueless_after_move()) {
      return;
    }

    if (alloc_traits::is_always_equal::value || _alloc == x._alloc) {
      _self = x._self;
      _self->count.fetch_add(1, std::memory_order_relaxed);
    } else {
      _self = _make_model(_alloc, *x);
    }
  }

  copy_on_write(copy_on_write&& other) noexcept
    : _alloc{other._alloc}
    , _self{std::exchange(other._self, nullptr)}
  {
  }

  ~copy_on_write()
  {
    assert(valueless_after_move() || _self->count > 0);
    if (_self != nullptr && _self->count.fetch_sub(1, std::memory_order_release) == 1) {
      std::atomic_thread_fence(std::memory_order_acquire);
      _destroy_model(_alloc, _self);
    }
  }

  auto operator=(copy_on_write const& x) -> copy_on_write&
  {
    static_assert(std::is_copy_constructible_v<value_type>);

    if (std::addressof(x) == this) {
      return *this;
    }

    constexpr bool pocca = alloc_traits::propagate_on_container_copy_assignment::value;

    if (x.valueless_after_move()) {
      _reset(nullptr);
    } else if (pocca || alloc_traits::is_always_equal::value || _alloc == x._alloc) {
      x._self->count.fetch_add(1, std::memory_order_relaxed);
      _reset(x._self);
    } else {
      _reset(_make_model(_alloc, *x));
    }

    if constexpr (pocca) {
      _alloc = x._alloc;
    }

    return *this;
  }

  auto operator=(copy_on_write&& x) noexcept(
    alloc_traits::propagate_on_container_move_assignment::value ||
    alloc_traits::is_always_equal::value) -> copy_on_write&
  {
    if (std::addressof(x) == this) {
      return *this;
    }

    constexpr bool pocma = alloc_traits::propagate_on_container_move_assignment::value;

    if (x.valueless_after_move()) {
      _reset(nullptr);
    } else if (pocma || _alloc == x._alloc) {
      _reset(std::exchange(x._self, nullptr));
    } else {
      _reset(_make_model(_alloc, std::move(x._self->value)));
      x._reset(nullptr);
    }

    if constexpr (pocma) {
      _alloc = x._alloc;
    }

    return *this;
  }

  template <typename U = value_type>
    requires(!std::same_as<std::remove_cvref_t<U>, copy_on_write> &&
             std::constructible_from<value_type, U> && std::assignable_from<value_type&, U>)
  auto operator=(U&& x) -> copy_on_write&
  {
    if (_self != nullptr && use_count() == 1) {
      _self->value = std::forward<U>(x);
      return *this;
    }

    return *this = copy_on_write(std::forward<U>(x));
  }

  //
  // Observers
  //

  auto operator*() const noexcept -> value_type const&
  {
    assert(!valueless_after_move());
    return _self->value;
  }

  auto operator->() const noexcept -> const_pointer
  {
    assert(!valueless_after_move());
    return std::pointer_traits<const_pointer>::pointer_to(_self->value);
  }

  [[nodiscard]] auto valueless_after_move() const noexcept -> bool { return _self == nullptr; }

  [[nodiscard]] auto get_allocator() const noexcept -> allocator_type { return _alloc; }

  [[nodiscard]] auto use_count() const noexcept -> long
  {
    assert(!valueless_after_move());
    return _self->count.load(std::memory_order_acquire);
  }

  [[nodiscard]] auto identical_to(copy_on_write const& x) const noexcept -> bool
  {
    assert(!valueless_after_move() && !x.valueless_after_move());
    return _self == x._self;
  }

  //
  // Modifiers
  //

  template <detail::cow_action<value_type> Action>
  void modify(Action&& action)
  {
    if (use_count() > 1) {
      auto* p = _make_model(_alloc, std::as_const(_self->value));
      _self->count.fetch_sub(1, std::memory_order_release);
      _self = p;
    }

    std::forward<Action>(action)(_self->value);
  }

  template <detail::cow_action<value_type> Action, detail::cow_transformation<value_type> Transform>
  void modify(Action&& action, Transform&& transform)
  {
    if (use_count() > 1) {
      auto* p =
        _make_model(_alloc, std::forward<Transform>(transform)(std::as_const(_self->value)));
      _self->count.fetch_sub(1, std::memory_order_release);
      _self = p;
    } else {
      std::forward<Action>(action)(_self->value);
    }
  }

  void swap(copy_on_write& other) noexcept(alloc_traits::propagate_on_container_swap::value ||
                                           alloc_traits::is_always_equal::value)
  {
    assert(alloc_traits::propagate_on_container_swap::value || _alloc == other._alloc);

    using std::swap;
    swap(_self, other._self);

    if constexpr (alloc_traits::propagate_on_container_swap::value) {
      swap(_alloc, other._alloc);
    }
  }

private:
  struct model
  {
    std::atomic<long> count;
    value_type value;
  };

  using model_alloc_t = typename alloc_traits::template rebind_alloc<model>;

  template <typename... Args>
  static auto _make_model(allocator_type& alloc, Args&&... args) -> model*
  {
    auto ma = model_alloc_t{alloc};
    auto* p = std::allocator_traits<model_alloc_t>::allocate(ma, 1);
    ::new (std::addressof(p->count)) std::atomic<long>{1};
    try {
      alloc_traits::construct(alloc, std::addressof(p->value), std::forward<Args>(args)...);
    } catch (...) {
      std::allocator_traits<model_alloc_t>::deallocate(ma, p, 1);
      throw;
    }
    return p;
  }

  static void _destroy_model(allocator_type& alloc, model* p)
  {
    auto ma = model_alloc_t{alloc};
    alloc_traits::destroy(alloc, std::addressof(p->value));
    std::allocator_traits<model_alloc_t>::deallocate(ma, p, 1);
  }

  void _reset(model* v)
  {
    if (_self != nullptr && _self->count.fetch_sub(1, std::memory_order_release) == 1) {
      std::atomic_thread_fence(std::memory_order_acquire);
      _destroy_model(_alloc, _self);
    }
    _self = v;
  }

  [[no_unique_address]] allocator_type _alloc;
  model* _self;
};

//
// Comparison operators
//

template <typename V1, typename V2>
auto operator==(copy_on_write<V1> const& x,
                copy_on_write<V2> const& y) noexcept(noexcept(*x == *y)) -> bool
{
  if (x.valueless_after_move() || y.valueless_after_move()) {
    return x.valueless_after_move() == y.valueless_after_move();
  }

  if constexpr (std::same_as<V1, V2>) {
    if (x.identical_to(y)) {
      return true;
    }
  }

  return *x == *y;
}

template <typename V, typename U>
  requires(!detail::is_copy_on_write_v<U>)
auto operator==(copy_on_write<V> const& x, U const& y) noexcept(noexcept(*x == y)) -> bool
{
  return !x.valueless_after_move() && (*x == y);
}

template <typename V1, typename V2>
auto operator<=>(copy_on_write<V1> const& x, copy_on_write<V2> const& y)
  -> detail::synth_three_way_result<typename copy_on_write<V1>::value_type,
                                    typename copy_on_write<V2>::value_type>
{
  if (x.valueless_after_move() || y.valueless_after_move()) {
    return !x.valueless_after_move() <=> !y.valueless_after_move();
  }

  if constexpr (std::same_as<V1, V2>) {
    if (x.identical_to(y)) {
      return std::strong_ordering::equal;
    }
  }

  return detail::synth_three_way(*x, *y);
}

template <typename V, typename U>
  requires(!detail::is_copy_on_write_v<U>)
auto operator<=>(copy_on_write<V> const& x, U const& y)
  -> detail::synth_three_way_result<typename copy_on_write<V>::value_type, U>
{
  if (x.valueless_after_move()) {
    return std::strong_ordering::less;
  }

  return detail::synth_three_way(*x, y);
}

template <typename V>
void swap(copy_on_write<V>& x, copy_on_write<V>& y) noexcept(noexcept(x.swap(y)))
{
  x.swap(y);
}

//
// Deduction guides
//

template <typename Value>
copy_on_write(Value) -> copy_on_write<indirect<Value>>;

template <typename Allocator, typename Value>
copy_on_write(std::allocator_arg_t, Allocator, Value)
  -> copy_on_write<indirect<Value,
                            typename std::allocator_traits<Allocator>::template rebind_alloc<Value>>>;

//
// PMR alias
//

namespace pmr {

template <typename T>
using copy_on_write =
  xyz::copy_on_write<xyz::indirect<T, std::pmr::polymorphic_allocator<T>>>;

} // namespace pmr

} // namespace xyz

//
// std::hash
//

template <typename V>
  requires requires(typename xyz::copy_on_write<V>::value_type t) { std::hash<decltype(t)>{}(t); }
struct std::hash<xyz::copy_on_write<V>>
{
  constexpr std::size_t operator()(xyz::copy_on_write<V> const& x) const
    noexcept(noexcept(std::hash<typename xyz::copy_on_write<V>::value_type>{}(*x)))
  {
    if (x.valueless_after_move()) {
      return static_cast<std::size_t>(-1);
    }

    return std::hash<typename xyz::copy_on_write<V>::value_type>{}(*x);
  }
};

#endif // XYZ_COMPOSABLE_VALUE_TYPES_H
