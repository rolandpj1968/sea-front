// EXPECT: 7
// Regression: overload-set lookup must not truncate. With many
// same-named overloads in scope, a fixed-cap stack buffer would
// drop the LIFO bucket chain past its end — typically the
// earliest-registered overloads (often a templated overload
// pulled in via a header). Symptom: overload resolution can't
// see the templated overload, the call mangles bare against an
// un-emitted symbol, and the program fails to link.
//
// Standard: N4659 §16.3 [over.match] requires the resolver to
// consider every viable candidate visible at the call site;
// a truncated lookup violates that.

template<typename T> int picker(T *p) { (void)p; return 7; }

void picker(int) {}    void picker(int, int) {}
void picker(char) {}   void picker(char, int) {}
void picker(short) {}  void picker(short, int) {}
void picker(long) {}   void picker(long, int) {}
void picker(float) {}  void picker(float, int) {}
void picker(double) {} void picker(double, int) {}
void picker(unsigned) {} void picker(unsigned, int) {}
void picker(signed char) {} void picker(signed char, int) {}
void picker(unsigned char) {} void picker(unsigned char, int) {}
void picker(unsigned short) {} void picker(unsigned short, int) {}

struct Bar {};

int main() {
    Bar b;
    return picker(&b);   /* must reach the templated overload */
}
