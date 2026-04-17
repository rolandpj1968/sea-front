// EXPECT: 42
// Test: template instantiation inside a typedef'd struct.
// 'typedef struct S { vec<T> member; } S2;' — the struct body is only
// accessible through the Type's class_def (no separate ND_CLASS_DEF
// in the TU), so collection must walk into typedef'd class bodies.
template<typename T> struct vec { T data; int len; };
typedef const char *str;
typedef struct S { const char *key; vec<str> mangled; } S2;
int main() {
    S2 s;
    s.mangled.len = 42;
    return s.mangled.len;
}
