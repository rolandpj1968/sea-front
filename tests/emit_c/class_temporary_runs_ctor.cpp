// EXPECT: 0
// `T{};` and `T();` as functional-cast value-init must run the
// class's default ctor when one exists (user-declared, or
// synthesised because a member / base needs construction).
// sea-front's compound-literal lowering `(struct T){0}` alone
// bypasses every ctor, leaving the synthesised side-effects
// unobserved. N4659 §8.2.3/2 [expr.type.conv] + §15.6.2
// [class.base.init].
//
// Coverage:
//   - User-declared default ctor with side effect (consteval-style).
//   - Synthesised default ctor (implicit) chaining into a member's
//     user ctor — the reduced shape from g++.dg/init/pr64527.C.
//
// Dtor at end-of-full-expression is NOT modelled here yet (would
// need full-expression-end machinery to be safe for non-discarded
// temporaries).

extern "C" void abort(void);

int g_user_ctor = 0;
int g_member_ctor = 0;

struct UserDtor {
    UserDtor() { ++g_user_ctor; }
};

struct Member {
    Member() { ++g_member_ctor; }
};

struct Aggregate {
    Member m;
};

int main() {
    (void) UserDtor{};
    if (g_user_ctor != 1) abort();
    (void) Aggregate{};
    if (g_member_ctor != 1) abort();
    return 0;
}
