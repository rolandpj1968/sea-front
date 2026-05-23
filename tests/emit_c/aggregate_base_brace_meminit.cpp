// EXPECT: 5
// Brace-init of an aggregate base in the mem-init list:
//   Derived(int x) : Aggregate{ x } {}
// Aggregate has no user-declared ctors so the brace-init is
// C++11 aggregate initialization (N4659 §11.6.1 [dcl.init.aggr])
// applied to a base subobject (§15.6.2/4 — bases may be aggregate-
// initialized in the mem-init list).
//
// Sea-front previously routed every 'Base(args...)' or 'Base{args...}'
// mem-init through ctor overload resolution; with no matching ctor
// for an aggregate base, it aborted in die_no_overload. The fix is
// to fall back to field-by-field assignment of the base's data
// members when no ctor resolves.
//
// Pattern from glibc <bits/atomic_base.h> atomic_flag:
//   constexpr atomic_flag(bool __i) noexcept
//     : __atomic_flag_base{ _S_init(__i) } { }
// where __atomic_flag_base has a single int data member and no
// user-declared ctors.

struct Aggregate {
    int value;
};

struct Derived : Aggregate {
    Derived(int x) : Aggregate{ x } {}
};

int main() {
    Derived d(5);
    return d.value;
}
