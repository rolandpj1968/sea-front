// EXPECT: 12
// Mixed: a class with a NON-default user ctor (Outer(int)) and
// a member needing a default ctor. Per N4659 §15.1, the user
// ctor suppresses the implicit default ctor — so 'Outer o;'
// (no args) is ill-formed in C++.
//
// We don't error today — has_default_ctor stays false because
// any_user_ctor is true. 'Outer o;' compiles but no Outer_ctor
// is called. Test verifies the alternative path: 'Outer o(0);'
// invokes the user ctor, which gets the synthesized member-init
// for Inner via emit_ctor_member_inits — no explicit mem-init
// list, but Inner has has_default_ctor so it auto-chains.
//
// Inner ctor: g = g*10 + 1 → g = 1
// Outer ctor body: g = g*10 + 2 → g = 12
int g = 0;

struct Inner { Inner() { g = g * 10 + 1; } };

struct Outer {
    Inner i;
    Outer(int x) { g = g * 10 + 2; }
};

int main() {
    Outer o(0);
    return g;
}
