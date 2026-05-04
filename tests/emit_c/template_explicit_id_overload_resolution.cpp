// EXPECT: 7
// Explicit-template-id call 'f<Bag, VA>(...)' must resolve to the
// overload whose function-parameter list matches the call's argument
// shape. With multiple same-named templates of differing arity in
// scope (mirrors gcc 4.8 vec.h's gt_pch_nx + vec_alloc cluster),
// sea-front previously fell back to "first matching name" and could
// pick a 2-param overload for a 3-arg call, mangling the call to
// the wrong symbol.
//
// N4659 §16.3 [over.match] + §17.8.2.1 [temp.deduct.call]: candidate
// templates whose deduced signature doesn't have the right arity are
// removed; remaining are ranked by ICS.

struct Bag { int x; };
typedef void (*op_t)(void *, void *);
struct VA {};

template<typename T, typename A>
struct Vec {
    T data[2];
    int n;
    T &operator[](unsigned i) { return data[i]; }
};

template<typename T, typename A> void f(Vec<T, A> *v) { (void)v; }
template<typename T, typename A>
void f(Vec<T *, A> *v, op_t, void *) { (void)v; }
template<typename T, typename A>
void f(Vec<T, A> *v, op_t, void *) {
    extern void f(T *, op_t, void *);
    for (int i = 0; i < v->n; ++i)
        f(&((*v)[i]), (op_t)0, (void *)0);
}
template<typename T, typename A>
void f(Vec<T, A> *&v, unsigned n) { (void)v; (void)n; }

void f(Bag *b, op_t, void *) { b->x = 7; }

int main() {
    Vec<Bag, VA> v;
    v.n = 1;
    v.data[0].x = 0;
    f<Bag, VA>(&v, (op_t)0, (void *)0);
    return v.data[0].x;
}
