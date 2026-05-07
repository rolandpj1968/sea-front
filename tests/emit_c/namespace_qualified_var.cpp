// EXPECT: 42
// Namespace-qualified value access — N4659 §10.3 [basic.namespace].
// C has no namespaces; sea-front flattens 'namespace foo { int x; }'
// to TU-scope 'int x;', so 'foo::x' lowers to bare 'x' in the
// emitted C. Real-world hit: gcc 14 libcpp/lex.cc references
// 'bidi::utf8_start' through a similar namespace-qualified shape.

namespace foo {
    constexpr int x = 42;
}

int main() {
    return foo::x;
}
