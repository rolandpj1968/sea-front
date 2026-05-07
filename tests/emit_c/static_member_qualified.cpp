// EXPECT: 42
// 'Class::static_member' — qualified-id reference to a static data
// member (N4659 §8.1.4.3 [expr.prim.id.qual] + §9.4.2 [class.static.
// data]). Sema resolves the qualified lookup; codegen rewrites the
// ND_QUALIFIED node to the TU-scope mangled symbol
// 'sf__<class>__<name>', mirroring the implicit-this and ND_MEMBER
// rewrites covered by static_member_const.cpp.

struct K {
    static const int magic = 42;
};

int main() {
    return K::magic;
}
