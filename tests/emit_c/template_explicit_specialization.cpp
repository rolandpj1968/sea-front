// EXPECT: 3
// Explicit full specializations of a template member must mangle
// distinctly per template argument — the class tag in the mangled
// symbol must include the specialization's template arg so
// 'is_a_helper<A>::test' and 'is_a_helper<B>::test' produce different
// C symbols. Previously both variants mangled identically (using the
// primary template's tag), causing 'conflicting types' errors at the
// C compiler. N4659 §17.7.3 [temp.expl.spec]. Pattern from gcc 4.8
// cgraph.h's is_a_helper<cgraph_node> / is_a_helper<varpool_node>.
struct Apple  { int kind; };
struct Banana { int kind; };

template<typename T>
struct Classifier {
    static int identify(int);
};

template<> int Classifier<Apple>::identify(int v)  { return v + 1; }
template<> int Classifier<Banana>::identify(int v) { return v + 2; }

int main() {
    // Classifier<Apple>::identify(0) + Classifier<Banana>::identify(0)
    // = 1 + 2 = 3.
    return Classifier<Apple>::identify(0) + Classifier<Banana>::identify(0);
}
