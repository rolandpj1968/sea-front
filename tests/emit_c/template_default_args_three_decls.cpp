// EXPECT: 12
// N4659 §17.6.4/10 [temp.arg.default]: default arguments accumulate
// across more than two declarations of the same primary template.
// Three declarations each contribute one default; usage `Triple<>`
// must succeed because the merged set covers all three params.

template<class T1, class T2, class T3 = int>
struct Triple;

template<class T1, class T2 = int, class T3>
struct Triple;

template<class T1 = int, class T2, class T3>
struct Triple {
    T1 a;
    T2 b;
    T3 c;
};

int main() {
    Triple<> t = { 3, 4, 5 };
    return t.a + t.b + t.c; // 3 + 4 + 5 = 12
}
