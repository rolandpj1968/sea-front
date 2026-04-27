// EXPECT: 0
// Two overloads differing only in pointer depth — 'f(T*)' and 'f(T**)' —
// must resolve correctly based on the call's argument type. Sea-front's
// overload_match_score only compared the OUTER type kind (TY_PTR vs
// TY_PTR → equal score) and picked the first candidate by table order
// regardless of base. Now scores recurse into TY_PTR/TY_REF/TY_ARRAY
// bases so deeper structural agreement outranks shallow kind agreement.
//
// Surfaced by gcc 4.8 vec.h's overloaded 'iterate(unsigned, T*)' and
// 'iterate(unsigned, T**)' — sea-front always picked the T* overload
// even when the call argument was T**, leading to a stack smash in
// genopinit at runtime (the iterate write past the end of the smaller
// destination, corrupting the stack canary).
template<typename T>
struct Vec {
    T data;
    bool iterate(unsigned ix, T *out) const  { (void)ix; *out = data;     return true; }
    bool iterate(unsigned ix, T **out) const { (void)ix; *out = (T*)&data; return true; }
};

int main() {
    Vec<int> v;
    v.data = 42;
    int x = 0;
    int *xp = 0;
    v.iterate(0, &x);    // resolves to iterate(unsigned, T*)
    v.iterate(0, &xp);   // resolves to iterate(unsigned, T**)
    return (x == 42 && xp != 0 && *xp == 42) ? 0 : 1;
}
