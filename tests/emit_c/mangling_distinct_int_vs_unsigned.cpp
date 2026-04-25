// EXPECT: 12
// Distinction rule: free-function overload — N4659 §16.3 [over.match].
// f(int) and f(unsigned) are distinct overloads. If sea-front
// collapses them (same C symbol), either the second def causes
// "redefinition" at C-compile, or one body wins both calls and the
// result is wrong. Reference oracle: g++ produces two distinct
// mangled symbols (_Z1fi vs _Z1fj).

int f(int x)      { return x + 1;  }
int f(unsigned x) { return x + 10; }

int main() {
    int      a = 1;
    unsigned b = 0u;
    return f(a) + f(b);   // f(int)=2 + f(unsigned)=10 = 12
}
