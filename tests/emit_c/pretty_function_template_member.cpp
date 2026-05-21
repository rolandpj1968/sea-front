// EXPECT: 0
// __PRETTY_FUNCTION__ inside a class-template member must produce
// a gcc-format string of the form
//   <ret-ty> <class>::<name>(<params>) [with T = <conc>; ...]
// where the class qualifier uses the SOURCE-level template-param
// names (e.g. `X<T>`) and the substitutions are listed in the
// `[with ...]` suffix. Ctors and dtors omit the return type.
// N4659 §8.4.1/8 [dcl.fct.def].
//
// Mirrors g++.dg/template/pretty1.C.

extern "C" int strcmp(const char *, const char *);

static int errs = 0;

static void check(const char *got, const char *want) {
    if (strcmp(got, want) != 0) ++errs;
}

template <typename T>
struct X {
    X()  { check(__PRETTY_FUNCTION__, "X<T>::X() [with T = void]"); }
    ~X() { check(__PRETTY_FUNCTION__, "X<T>::~X() [with T = void]"); }
};

int main() {
    {
        X<void> x;
    }
    return errs;
}
