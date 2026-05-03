# copy_on_write design notes

## Goal

Extend `copy_on_write.hpp` so it can be used composably with `indirect<T>` and
`polymorphic<Base>` from jbcoe/value_types, picking up their ownership and
allocation models without duplicating logic.

---

## Attempt 1 — composable_value_types.h with copy_on_write<V>

Introduced a new `copy_on_write<V>` template in `composable_value_types.h`
where V = `indirect<T, A>`. V provides `value_type` and `allocator_type`; the
model stores V directly.

**Problem:** double indirection.

```
copy_on_write._self → model { count, indirect<T,A> { T* } } → T
```

Two heap pointer chases to reach T.

---

## Attempt 2 — P2047R7 / basic_optional pattern

Following [P2047R7](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p2047r7.html),
`copy_on_write<V>` stores its own `allocator_type _alloc` member (like
`basic_optional<T, Alloc>`) and uses `std::uses_allocator_construction_args<V>`
to forward the allocator into V during construction. V handles all allocation
for its contained value; `copy_on_write` manages the ref count and propagation
(POCCA/POCMA/POCS).

### Why the allocator must be stored in the class

Three reasons:

1. **Valueless state.** After a move `_self == nullptr`. The object is valueless
   but must still have an accessible allocator — `get_allocator()` must work,
   and any subsequent value assignment needs to know which allocator to use.

2. **Shared ownership, independent allocator identity.** Multiple
   `copy_on_write` objects can share one model. When one unshares (via `modify`
   or value assignment) it must allocate a new model using *its* allocator. The
   allocator is a property of each object, not of the shared model.

3. **The C++ allocator model.** POCCA, POCMA, and POCS are operations on the
   *container's* allocator. They update `_alloc` independently of what happens
   to the stored value.

### Why `model* _self` causes double indirection

Storing V = `indirect<T, A>` inside the model gives:

```
copy_on_write._self (ptr) → model { count, V { ptr } } → T
```

Two pointer chases. The original `copy_on_write.hpp` avoids this by storing T
inline in the model:

```
copy_on_write._self (ptr) → model { count, T }
```

Storing V in the model adds a redundant level: indirect already has a pointer to
T, and the model pointer is another on top.

---

## Revised goal — modify copy_on_write.hpp directly

`composable_value_types.h` was the wrong direction. The original
`copy_on_write.hpp` already stores T inline (single indirection). The one
missing piece is **allocator forwarding into T during construction**.

`_make_model` currently calls:

```cpp
alloc_traits::construct(a, std::addressof(p->value), std::forward<Args>(args)...);
```

This uses `a` to allocate the model but does not forward `a` into T's
constructor. Replacing this with `std::uses_allocator_construction_args<T>`
automatically routes the allocator to T (e.g. `indirect`, `polymorphic`,
`pmr::string`) if T declares `allocator_type`:

```cpp
std::apply(
    [&](auto&&... vargs) {
        alloc_traits::construct(a, std::addressof(p->value),
                                std::forward<decltype(vargs)>(vargs)...);
    },
    std::uses_allocator_construction_args<T>(a, std::forward<Args>(args)...));
```

With this change `copy_on_write<polymorphic<Base, A>>` and
`copy_on_write<indirect<T, A>>` both work correctly with allocator propagation,
no new file required.

---

## Remaining concern — double indirection with polymorphic

`copy_on_write<polymorphic<Base>>` is correct but has double indirection:
`polymorphic<Base>` has its own heap pointer to the derived object. For
performance-sensitive cases a dedicated type is better.

### copy_on_write_polymorphic

Merge the ref count and the derived object into a single heap allocation using a
virtual model base:

```
_self → model_derived<Derived> { count, [vtable], Derived_fields... }
```

```cpp
template <typename Base>
class copy_on_write_polymorphic {
    struct model_base {
        std::atomic<long> count{1};
        virtual ~model_base() = default;
        virtual Base const& get() const noexcept = 0;
        virtual model_base* clone() const = 0;
    };

    template <typename Derived>
    struct model_derived : model_base {
        Derived value;
        Base const& get() const noexcept override { return value; }
        model_base* clone() const override { return new model_derived{value}; }
    };

    model_base* _self;
};
```

One pointer dereference + vtable dispatch, no second heap allocation.

---

## Composable design — ownership vs storage as orthogonal concerns

The ref-counting machinery (increment, decrement, share on copy, unshare on
modify, POCCA/POCMA/POCS) is completely independent of how the value is stored.
A composable design separates these:

**Ownership** — lives in `copy_on_write`, unchanged regardless of storage.

**Storage policy** — how the heap node lays out the value. Must provide:
- a `model` type (heap node carrying the ref count)
- `make_model(args...)` — factory
- `destroy_model(model*)` — cleanup
- `clone_model(model*)` — deep copy for unsharing
- `get(model*)` — value access

Two concrete policies:

| Policy | Layout | Dispatch |
|--------|--------|----------|
| Inline | `{ count, T }` | direct |
| Virtual | `{ count, vtable, Derived }` | virtual |

```
copy_on_write<T>                 → inline policy   → single indirection, non-virtual
copy_on_write_polymorphic<Base>  → virtual policy  → single indirection, vtable
copy_on_write<polymorphic<Base>> → inline policy   → double indirection, non-virtual (correct but slower)
```

Both share 100% of the ownership code. Adding a third strategy (e.g.
small-buffer-optimised polymorphic) requires only a new policy, not a new
ownership implementation.
