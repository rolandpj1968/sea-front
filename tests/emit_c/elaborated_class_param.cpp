// EXPECT: 7
// 'class T *' as an elaborated-type-specifier in a parameter
// declaration — N4659 §10.1.7.3 [dcl.type.elab] / §6.4.4
// [basic.lookup.elab]. The 'class' keyword names the tag explicitly
// (same as 'struct T *' but using the C++ 'class' spelling).
// Common in gcc 14 source: e.g. libcpp/include/line-map.h declares
// 'extern void linemap_init (class line_maps *set, ...)'.
class Foo {
public:
    int v;
};
int peek(class Foo *p) { return p->v; }
int main() {
    Foo f;
    f.v = 7;
    return peek(&f);
}
