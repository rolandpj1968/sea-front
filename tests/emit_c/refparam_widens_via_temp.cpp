// EXPECT: 0
// Type-mismatched lvalue passed to a T& parameter must materialize a
// temporary of T initialized from the converted arg (N4659 §11.6.3
// [dcl.init.ref]/5.2). Sea-front previously emitted '&arg' (taking
// the address of the int), passing int* into a function expecting
// long*. The C compiler warned with -Wincompatible-pointer-types
// and accepted; the callee then read sizeof(long)=8 bytes from a
// sizeof(int)=4-byte int — garbage in the top half.
//
// Pattern: gcc 4.8 genautomata's
//   static int undefined_vect_el_value;        // declared as int
//   vla_hwint_t vect;                          // vec<long>
//   vect.safe_push(undefined_vect_el_value);   // int into long&
// quick_push then dereferenced 8 bytes from 4-byte storage. The
// reads of 'undef' inside add_vect picked up garbage in the top
// half, gcc_assert(x >= 0) tripped on the negative bit pattern,
// gen-tool aborted at runtime.
//
// Fix: emit_arg_for_param materializes &((T){arg}) — a C99 compound
// literal — when arg's resolved_type kind differs from the ref
// param's base kind for scalar types. The compiler converts arg
// to T at the literal initialiser, then we take the temp's address.
template<typename T>
struct Vec {
    T data;
    void take_ref(const T &x) { data = x; }
    T get() const { return data; }
};

int main() {
    Vec<long> v;
    int small = 42;
    v.take_ref(small);   // int lvalue → const long& param
    return v.get() == 42L ? 0 : 1;
}
