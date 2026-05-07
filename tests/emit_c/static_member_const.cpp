// EXPECT: 50
// 'static const' / 'static constexpr' integral data members
// (N4659 §9.4.2/3, §10.1.5) — declarations with in-class
// initialisers are also definitions for integral / enum / literal
// types. Sea-front lowers them to TU-scope variables outside the
// C struct body ('static const int sf__<class>__<name> = init;')
// because C has no in-struct member initializers and no static
// fields.
//
// Verified through three reference paths: implicit-this (from
// inside a method), object access ('s.magic'), and a qualifier
// not exercised here ('S::magic' goes through ND_QUALIFIED, a
// separate emit path that's covered when the test suite runs
// on torture sources).

struct S {
    static const int magic = 42;
    static constexpr int double_magic = 84;
    int instance;
    int get() { return magic + instance; }
};

int main() {
    S s;
    s.instance = 8;
    int via_method = s.get();         // implicit-this → sf__S__magic
    int via_obj    = s.magic + 0;     // s.magic       → sf__S__magic
    (void)via_method; (void)via_obj;
    return s.magic + s.instance;       // 42 + 8 = 50
}
