// EXPECT: 0
// A noexcept / throw() function called WHILE an exception is
// already propagating must not trip its own no-throw guard merely
// by inheriting the in-flight state. N4659 §18.4 [except.spec] —
// the no-throw violation is "an exception escaping THE FUNCTION",
// not "an exception in flight when the function returns". Sea-
// front's epilogue check snapshots the entry-time state and only
// terminates when the state changed inside the function.
//
// Reproducer pattern: g++.dg/eh/new1.C — `::operator delete[]` is
// declared throw() and called from the new-expression's on-throw
// chain. Without the entry-state snapshot, sea-front would
// terminate the moment the dtor was invoked during unwind.

int dcount = 0;

struct Tracker {
    ~Tracker() throw() { ++dcount; }   // explicit throw() spec
};

struct Thrower { Thrower() { throw 1; } };

int main() {
    try {
        Tracker t;        // ctor: nothing.
        Thrower th;       // ctor throws — unwind starts.
        return 1;         // unreachable
    } catch (...) {
        // Tracker's dtor must have run during unwind without
        // tripping its throw() guard (which would __sf_terminate).
        if (dcount != 1) return 2;
    }
    return 0;
}
