// EXPECT: 42
// An instantiated template method whose parameter type is an enum
// pointer — e.g. 'bool iterate(unsigned, T*) const' with T=enum —
// must have its forward declaration emitted AFTER the enum body.
//
// Before the fix, the method-forward-decl sweep ran BEFORE the enum-
// body pass, so 'enum Color *' appeared in the forward decl against
// an undeclared enum tag. With -std=c99, gcc accepts this under an
// implicit-enum rule but then flags the later real enum body (and
// the method definition) as 'conflicting types' — errors whose
// printed types look identical because the conflict lives in the
// enum-complete vs enum-incomplete bookkeeping.
//
// Pattern from gcc 4.8 vec.h: vec<ld_plugin_symbol_resolution, ...>::
// iterate(unsigned, enum *) const. N4659 §6.7.2.3, C11 §6.7.2.3.
enum Color { RED, GREEN, BLUE };

template<typename T>
struct vec {
    T *data_;
    int len_;
    bool iterate(unsigned ix, T *p) const {
        if (ix < (unsigned)len_) { *p = data_[ix]; return true; }
        return false;
    }
};

int main() {
    Color arr[3] = { RED, GREEN, BLUE };
    vec<Color> v; v.data_ = arr; v.len_ = 3;
    Color c;
    return (v.iterate(1, &c) && c == GREEN) ? 42 : 0;
}
