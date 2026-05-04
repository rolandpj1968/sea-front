// EXPECT: 7
// N4659 §17.6.4/10 [temp.arg.default]: default template arguments
// merge across multiple primary declarations of the same template.
// Standard example:
//   template<class T1, class T2 = int> class A;       // (1) T2=int
//   template<class T1 = int, class T2> class A;       // (2) T1=int
// `A<>` is then equivalent to `A<int, int>`.
//
// Sea-front parses each declaration into a separate ND_TEMPLATE_DECL.
// `find_primary_template_in_scope` walks every primary and merges
// their default_type entries onto the canonical head, so a usage
// like `Box<>` triggers default expansion using the union of all
// declared defaults. Without the merge, only the canonical
// declaration's defaults apply and `Box<>` either fails to expand
// or expands using the wrong argument count.

template<class T1, class T2 = int>
struct Box;

template<class T1 = int, class T2>
struct Box {
    T1 a;
    T2 b;
};

int main() {
    Box<> b = { 3, 4 };  // both defaults must come from across the decls
    return b.a + b.b;    // expect 3 + 4 = 7
}
