// EXPECT: 42
// Reduce gcc 4.8 vec.h's safe_splice/splice pattern. A method 'do_op'
// inside a partial spec calls a same-class method 'op' unqualifiedly.
// There is also a free function template named 'op' in the surrounding
// scope (mimics glibc's <fcntl.h>::splice syscall + class-scope splice).
//
// Per N4659 §6.4.1/8 [basic.lookup.unqual], unqualified lookup inside
// a class member function searches the class scope first, then the
// enclosing namespaces. The class-scope op should win — and the call
// must mangle as 'sf__Box_t..._te___op_..._pe_(this, ...)', NOT as a
// free-function-template instantiation 'op_t_..._te__p_..._pe_(...)'.
//
// The bug this guards against: sema's bare-ident-template rewrite
// (visit_call ~line 1518) misclassifies the unqualified op() as a
// function-template call when a free-function template of the same
// name is reachable. The rewrite synthesises ND_TEMPLATE_ID and the
// instantiation pipeline emits a free-function symbol — leaving the
// real method-call mangle unsatisfied.

template<typename T>
int op(T x);                                    // free template DECL

template<typename T, typename L>
struct Box;

template<typename T>
struct Box<T, int> {
    T val;
    int op(int extra);                          // method (in-class decl)
    int do_op(int delta) { return op(delta); }  // calls SAME-CLASS op
};

template<typename T>
int Box<T, int>::op(int extra) {
    return (int)val + extra;
}

template<typename T>
int op(T x) { return (int)x; }

int main() {
    Box<int, int> b;
    b.val = 40;
    return b.do_op(2);
}
