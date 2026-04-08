// EXPECT: 7
// Default ctor synthesis: a class with no user-declared ctors
// but with a member that DOES need a default ctor gets a
// synthesized Class_ctor(&this) that chains into the member's
// ctor. Mirrors the dtor synthesis we already do.
//
// Outer has no ctor source. type.c sees Inner has has_default_ctor
// and propagates has_default_ctor to Outer with no_user_ctor=true.
// emit_class_def synthesizes Outer_ctor with body { Inner_ctor(&this->i); }.
// 'Outer o;' in main triggers the auto-call.
//
// Pre-fix: Outer had has_default_ctor=false (only set on user
// zero-arg ctor). 'Outer o;' did nothing, Inner_ctor never ran,
// g stayed 0, exit was 0.
int g = 0;

struct Inner {
    Inner() { g = 7; }
};

struct Outer {
    Inner i;
};

int main() {
    Outer o;
    return g;
}
