// EXPECT: 0
// __func__ / __FUNCTION__ pre-defined identifier — N4659 §8.4.1/8
// [dcl.fct.def]: implicitly defined as the unqualified function name.
//
// The host C compiler also implements __func__, but expands it to the
// C function name — which for sea-front is the MANGLED symbol (e.g.
// '_ZN3y8a4zqjxEic' instead of 'zqjx'). To match C++ semantics we
// substitute a string literal of the user-written source name during
// emit; destructors get a '~' prefix since their func node name
// token holds the class name without it.
//
// Patterns: g++.dg/ext/fnname2.C (member), fnname3.C (ctor/dtor).

bool str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == 0 && *b == 0;
}

const char *plain_name() { return __func__; }

struct Cls {
    const char *member_name() const { return __func__; }
    static const char *ctor_name;
    static const char *dtor_name;
    Cls() { ctor_name = __func__; }
    ~Cls() { dtor_name = __func__; }
};

const char *Cls::ctor_name = 0;
const char *Cls::dtor_name = 0;

int main() {
    if (!str_eq(plain_name(), "plain_name")) return 1;
    Cls c;
    if (!str_eq(c.member_name(), "member_name")) return 2;
    if (!str_eq(Cls::ctor_name, "Cls")) return 3;
    // __FUNCTION__ is a gcc alias for __func__.
    const char *via_alias = __FUNCTION__;
    if (!str_eq(via_alias, "main")) return 4;
    return 0;
    // dtor runs after return; we can't observe it here, but the
    // ctor case already covers the source-name substitution path.
}
