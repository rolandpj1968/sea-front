// EXPECT: 42
// A class-template member's body references the OUTER class
// template parameter inside a local-variable type. The instantiator
// must seed the outer param→arg binding before cloning, otherwise
// the local-variable type leaks into emit as TY_DEPENDENT and the
// generated C is malformed.
//
// N4659 §17.5.2/2 [temp.mem]: a member of a class template is
// itself a template; its body sees both the outer and the inner
// parameters. §17.7.1 [temp.inst] requires substitution to apply
// to the entire body when the member is instantiated.

template<typename T>
struct Holder {
    T stored;

    template<typename U>
    int combine(U mul) {
        T local = stored;        // T appears only in body, not in the
        return (int)local * mul; // member-template's function-param types
    }
};

int main() {
    Holder<int> h;
    h.stored = 6;
    return h.combine<int>(7);
}
