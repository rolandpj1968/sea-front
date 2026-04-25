// EXPECT: 11
// Bare-ident free-function template whose body calls a method on
// its argument's instantiated class type. The vec.h shape:
//
//   template<typename T> T grab(Holder<T> *h) { return h->take(); }
//
// where Holder<T>::take() is a method. After visit_call rewrites the
// bare ident to ND_TEMPLATE_ID and the function template body is
// cloned with T=int, the inner call `h->take()` must lower to a call
// to the mangled Holder<int>::take method, NOT emit literal `h->take()`
// (which would be invalid C — structs don't have method members).

template<typename T>
struct Holder {
    T value;
    T take() { return value; }
};

template<typename T>
T grab(Holder<T> *h) { return h->take(); }

int main() {
    Holder<int> hi;
    hi.value = 11;
    return grab(&hi);   // T=int deduced; h->take() must dispatch.
}
