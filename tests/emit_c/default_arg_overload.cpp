// EXPECT: 83
// Default-argument expansion in overload resolution and at the
// call site — N4659 §11.3.6 [dcl.fct.default] + §16.3.1.4
// [over.match.viable]/2: a candidate is viable when nargs equals
// nparams OR when nargs < nparams and every excess param has a
// default. The call site fills the trailing slots from
// param.default_value.
//
// Real-world hit: gcc 14 libcpp's rich_location declared as
//   rich_location(line_maps*, location_t, const range_label* = NULL);
// is called from lex.cc / directives.cc / errors.cc with only the
// first two args.
//
// Audit coverage — exercises the four call shapes that each take a
// distinct codegen path. Sea-front's prior default-arg machinery
// only handled a subset (declarations of methods via TY_FUNC's
// param_defaults; ctor calls with full-arg counts), silently
// emitting too few args at any other shape.
//
//   1. ctor with deleted copy — multi-candidate resolution
//   2. free function call
//   3. member function call (inline-defined; the inline-def path
//      previously skipped defaults and was a latent silent bug)
//   4. qualified static method call (Class::stat())
//
// Sum of contributions: 11 (ctor) + 11 (free) + 55 (meth) +
//   14 (stat) = 92 if all defaults expand correctly. Wait — the
//   ctor sets z=99; the free fn returns 1+10=11; meth returns
//   50+5=55; stat returns 7*2=14. Plus over(1,2)=3 from the
//   already-working overload pair. Total 99-loc.z=99 isolated...
//
// Simpler rendering — capture each shape's contribution:
//   free_fn(1)      = 1 + 10 = 11
//   c.meth()        = 50 + 5 = 55
//   C::stat()       = 7 * 2  = 14
//   over(1, 2)      = 1 + 2  = 3
//   Total           = 11 + 55 + 14 + 3 = 83

int free_fn(int a, int b = 10) { return a + b; }

struct C {
    int v;
    C(int vv) : v(vv) {}
    int meth(int x = 5) { return v + x; }
    static int stat(int x = 7) { return x * 2; }
};

// Two overloads — exercises multi-candidate selection. The 2-arg
// form has its own default but the call supplies both args.
int over(int a)              { return a * 100; }
int over(int a, int b = 20)  { return a + b; }

// Ctor with default + deleted copy — the rich_location-style
// shape that triggers the multi-candidate viability path.
struct Loc {
    int x, y, z;
    Loc(int a, int b, int c = 99) : x(a), y(b), z(c) {}
    Loc(const Loc &) = delete;
};

int main() {
    int total = 0;
    total += free_fn(1);    // 11   — free function path
    C c(50);
    total += c.meth();      // 55   — member call path
    total += C::stat();     // 14   — qualified static path
    total += over(1, 2);    //  3   — multi-overload (full args)

    /* Verify the rich_location-style ctor independently. */
    Loc l(1, 2);
    if (l.z != 99) return -1;

    return total;           // 83
}
