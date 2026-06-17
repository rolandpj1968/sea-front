// EXPECT: 42
// A typedef of a const-qualified type carries the const through
// to use-sites — `typedef const int Cint; Cint x = 42;` declares
// a const int. Pre-fix, the parser's typedef-lookup path
// overwrote is_const from the use-site spec (false here), turning
// `Cint x` into a non-const int. Verified via reading the value
// back; if const-ness were dropped this test wouldn't FAIL, but
// it locks in the rule for the more visible TY_FUNC case in
// typedef_const_method.cpp.

typedef const int Cint;

Cint x = 42;

int main() { return x; }
