// EXPECT: 7
// Two classes where each method's signature references the OTHER
// class by pointer. Forward declarations must emit ALL struct/union
// predecls BEFORE any method forward decls — otherwise a later
// forward decl's parameter list references an un-predeclared struct
// tag, creating a param-list-scoped type that diverges from the
// file-scope definition ("conflicting types").
struct A;
struct B;

struct A {
    int v;
    int sum_with(B* b);   // references B before B is defined
};

struct B {
    int w;
    int sum_with(A* a);   // references A already defined
};

int A::sum_with(B* b) { return v + b->w; }
int B::sum_with(A* a) { return w + a->v; }

int main() {
    A a; a.v = 3;
    B b; b.w = 4;
    return a.sum_with(&b);   // 3 + 4 = 7
}
