// EXPECT: 12
// Distinction rule: cv-qualified member function — N4659 §16.2
// [over.load]. void f() and void f() const are distinct overloads;
// the const version is selected on const objects.

struct S {
    int v;
    int g()       { return v + 1; }
    int g() const { return v + 10; }
};

int main() {
    S s; s.v = 1;
    const S c = { 0 };  // c.v = 0
    return s.g() + c.g();   // 2 + 10 = 12
}
