// EXPECT: 0
// `using Base::Base;` inherits Base's constructors (N4659 §15.6.3
// [class.inhctor.init]). For `Derived d(arg)` where Derived has
// no matching ctor of its own but a Base ctor does, sea-front
// now forwards to the Base ctor on &d.__sf_base. Before, the
// direct-init fell through to aggregate-init `(struct Derived){arg}`
// — wrong shape, wrong types.
//
// Also tests the move-vs-copy ctor pick when the arg is a
// reference-cast: the move ctor `T(T&&)` must be selected over
// a deleted copy ctor `T(const T&) = delete`. resolve_overload
// is now called with the rvalref-preserving source type so the
// match picks the rvalref-param ctor.
//
// Reduced from g++.dg/cpp1z/inh-ctor38.C.

extern "C" void abort(void);

static int moves = 0;

struct Ptr {
    int *p = nullptr;
    Ptr() {}
    Ptr(Ptr const &) = delete;
    Ptr(Ptr&& other) : p(other.p) { ++moves; }
};

struct Base {
    Ptr val;
    Base(Ptr val_);
};
Base::Base(Ptr val_) : val(static_cast<Ptr&&>(val_)) {}

struct Derived : Base {
    using Base::Base;
};

int main() {
    Ptr ptr;
    Derived d(static_cast<Ptr&&>(ptr));
    if (d.val.p != nullptr) abort();
    /* Two moves: ptr → ctor-arg val_, then val_ → Base.val. */
    if (moves != 2) abort();
    return 0;
}
