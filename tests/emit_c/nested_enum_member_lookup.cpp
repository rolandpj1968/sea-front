// EXPECT: 3
// N4659 §10.2 [dcl.enum] / §6.4.3 [basic.lookup.qual]: enumerators of
// a nested enum are class-scope named constants, not data members. An
// unqualified reference inside a method body must NOT be rewritten to
// 'this->ENUMERATOR' — there is no such field. Real-world hit: gcc 14
// libcpp/macro.cc class vaopt_state's nested enum update_type, used
// as a bare 'INCLUDE'/'ERROR'/'BEGIN' inside vaopt_state::update().
struct S {
    enum Kind { A = 1, B = 2, C = 3 };
    Kind state;
    S() : state(A) {}
    int classify(int x) {
        if (x == 1) { state = A; return A; }
        if (x == 2) { state = B; return B; }
        state = C;
        return C;
    }
};

int main() {
    S s;
    return s.classify(0); // C = 3
}
