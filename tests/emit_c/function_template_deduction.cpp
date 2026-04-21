// EXPECT: 3
// Function template with a deducible parameter:
//   template<typename T, typename U> bool is_a(U* p);
// Called as 'is_a<Cat>(&thing)' — only T is explicit; U must be
// deduced from the call argument type. Without deduction-in-
// instantiation, same-T-different-U calls collapsed to ONE
// instantiation named 'sf__is_a_t_<T>_te_', leaving U unsubstituted
// (/*dep:U*/) and producing 'conflicting types' errors when the
// callers passed differently-typed pointers.
//
// N4659 §17.8.2.1 [temp.deduct.call]: template parameters not
// explicitly given are deduced from the function call.
// Pattern from gcc 4.8 is-a.h's is_a / dyn_cast templates.
struct Cat {};

template<typename T, typename U>
inline bool is_a(U* p) { return p != 0; }

struct Thing {};
struct Other {};

int main() {
    Thing x;
    Other y;
    int r = 0;
    // Same T=Cat, U deduced differently — must produce distinct mangled names.
    if (is_a<Cat>(&x)) r += 1;
    if (is_a<Cat>(&y)) r += 2;
    return r;  // 3
}
