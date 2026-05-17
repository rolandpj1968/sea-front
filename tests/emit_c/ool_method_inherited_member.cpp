// EXPECT: 42
// OOL method bodies must resolve inherited members through the
// __sf_base chain — N4659 §10.2 [class.member.lookup] /
// §11 [class.derived]. The implicit-this rewriter needs the
// enclosing class context (g_current_class_def) set for OOL methods,
// not just OOL constructors; otherwise unqualified base-member
// references like 'b' in 'int D::get() { return b; }' emit raw
// 'this->b' and the C compiler rejects with "no member named b".

struct Base { int b; };

struct Derived : Base {
    int get() const;       // declared in-class, defined out-of-line
};

int Derived::get() const { return b; }   // 'b' inherited from Base

int main() {
    Derived d = { { 42 } };  // aggregate init dodges the base-ctor-w/-args bug
    return d.get();
}
