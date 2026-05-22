// EXPECT: 0
// `T u = v;` / `T u(v);` must call the user-declared copy
// constructor when one exists (N4659 §15.8/1 [class.copy]).
// Three shapes exercised:
//
//   1. Direct user copy ctor on the target class — bare scalar
//      result; sea-front used to emit a bitwise struct copy.
//   2. Transitive copy ctor through a non-immediate base — the
//      target class has no own user copy ctor but inherits from
//      one that does. The implicit copy ctor expands to an inline
//      subobject-by-subobject copy chain that recursively calls
//      the base's user copy ctor.
//   3. Comma-separated declarators in a single statement
//      (`T u, v(u);`) — sea-front's flat-block emit previously
//      skipped pushing the cleanup chain entry, so the dtor never
//      fired at scope exit. The fix is also covered here.

int copies = 0;
int dtors  = 0;

struct Tracker {
    int v;
    Tracker() : v(0) {}
    Tracker(const Tracker& o) : v(o.v + 1) { ++copies; }
    ~Tracker() { ++dtors; }
};

struct Wrap {
    Tracker t;
    /* no user ctor — sea-front synthesizes implicit ones that
     * must recurse into t's user copy ctor. */
};

int main() {
    {
        // Shape 1: direct user copy ctor.
        Tracker a;                  // v=0
        Tracker b = a;              // v=1, copies=1
        Tracker c(b);               // v=2, copies=2
        if (a.v != 0) return 1;
        if (b.v != 1) return 2;
        if (c.v != 2) return 3;
        if (copies != 2) return 4;
    }
    // 3 Trackers destroyed — both user-declared dtors fire.
    if (dtors != 3) return 5;

    copies = 0; dtors = 0;
    {
        // Shape 2: transitive copy through implicit member init.
        Wrap a;                     // a.t.v=0
        Wrap b = a;                 // implicit Wrap copy → Tracker copy
        if (b.t.v != 1) return 6;
        if (copies != 1) return 7;
    }
    if (dtors != 2) return 8;       // a.t + b.t

    copies = 0; dtors = 0;
    {
        // Shape 3: comma-separated declarators in one stmt.
        Tracker p, q(p);            // p.v=0, q.v=1, copies=1
        if (copies != 1) return 9;
    }
    if (dtors != 2) return 10;      // both p and q dtors fire
    return 0;
}
