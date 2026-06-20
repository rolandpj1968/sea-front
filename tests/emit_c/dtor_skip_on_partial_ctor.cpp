// EXPECT: 0
// N4659 §15.2/2 [except.ctor]: an object that is partially
// constructed has destructors executed for all of its fully
// constructed subobjects, but NOT for itself. When a ctor body
// throws partway through, the object isn't fully constructed —
// the cleanup chain must skip the dtor on that var.
//
// Sea-front's emit unconditionally ran the dtor for every local
// class var in scope on cleanup, even ones whose ctor had just
// thrown. For an ex-like class that reads through an uninitialised
// pointer in its dtor, that's a segfault.
//
// The cleanup-chain machinery now installs a ctor-completed guard:
//   int __sf_ctor_ok_<N> = 0;
//   T x(args);     // ctor — may throw; doesn't reach line below if so
//   if (state != THROW) __sf_ctor_ok_<N> = 1;
//   ...
//   if (__sf_ctor_ok_<N>) ~T(&x);   // cleanup chain
//
// The guard is allocated per CL_VAR cleanup entry via
// prepare_ctor_guard / commit_ctor_guard, gated on the class
// having a dtor + the var-decl having a ctor-call init (the
// trivial cases — value-init, aggregate-init, hoisted temp —
// don't have a partial-construction transition).

extern "C" void abort();

int dtor_calls = 0;

struct Bomb {
    int *p;
    Bomb(int dummy) {
        /* Throw before `p` is initialised. */
        (void)dummy;
        throw 1;
    }
    ~Bomb() {
        ++dtor_calls;
        /* If the cleanup chain mistakenly runs ~Bomb on a
         * partially-constructed object, *p segfaults
         * (uninitialised pointer). */
        *p = 0;
    }
};

int main() {
    try {
        Bomb b(7);
    } catch (int) {}

    /* No dtor should have fired — the ctor never completed. */
    if (dtor_calls != 0) abort();

    return 0;
}
