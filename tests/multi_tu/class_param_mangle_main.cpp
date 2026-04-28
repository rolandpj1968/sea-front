// EXPECT: 9
// Companion to class_param_mangle_a.cpp. This TU sees TWO
// overloads of f — the class-param one (defined in _a.cpp) and
// an int-only one defined locally. The int-only one IS used so
// it's not DCE'd. The call site f(&b) is the cross-TU one we
// care about: this TU mangles the call (sees overloads), the
// other TU must mangle the def (class-param shape forces it).

struct Box { int v; };

extern int f(Box *p);   // defined in _a.cpp — must mangle
static int f(int x) { return x + 1; }   // local overload (forces this TU to mangle)

int main() {
    Box b; b.v = 5;
    return f(&b) + f(3);   // 5 + 4 = 9
}
