// Demo: templates (N4659 §17 [temp]).
//
// Sea-front instantiates templates at the AST level: it clones the
// template's tree, substitutes the parameter types, and prepends the
// concrete instantiation to the translation unit. Mangling encodes
// the template arguments, e.g.   Box<int>  ->  sf__Box_t_int_te_
//
// Each (template, arg-tuple) is instantiated once and deduplicated.

template<typename T>
struct Box {
    T val;
    Box(T v) : val(v) {}
    T get() { return val; }
};

template<typename T>
T max(T a, T b) { return a > b ? a : b; }

int main() {
    Box<int>  bi(40);
    Box<char> bc('A');                    // 65

    // Two distinct max() instantiations: max<int> and max<char>.
    int m1 = max(bi.get(), 30);           // 40
    int m2 = max((int)bc.get(), 60);      // 65

    return m2 - m1 - 24;                  // 1
}
