// EXPECT: 123
// User-declared dtor + multiple non-trivial members. Verifies the
// full destruction order N4659 §15.4 [class.dtor]/9 specifies:
//
//   1. Run the user-declared dtor body.
//   2. Then destroy non-static members in REVERSE declaration
//      order (no bases here).
//
// Layout:
//   struct Outer {
//       Member1 a;
//       Member2 b;
//       ~Outer() { /* runs first */ }
//   };
//
// IDs reflect the FIRING order, not the declaration order:
//   ~Outer body fires first  → id=1
//   ~Member2 fires second    → id=2  (last declared, destroyed first)
//   ~Member1 fires third     → id=3  (first declared, destroyed last)
//
// Each step does g = g*10 + id, building 1 → 12 → 123. The
// final value 123 fits in an 8-bit exit code.
int g = 0;

struct Member1 { ~Member1() { g = g * 10 + 3; } };
struct Member2 { ~Member2() { g = g * 10 + 2; } };

struct Outer {
    Member1 a;
    Member2 b;
    ~Outer() { g = g * 10 + 1; }
};

int main() {
    {
        Outer o;
    }
    return g;
}
