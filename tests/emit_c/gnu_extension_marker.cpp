// EXPECT: 42
// GNU '__extension__' marker — non-standard no-op that disables
// pedantic warnings in g++ for the immediately-following expression.
// Pervasive in glibc/libiberty macros (XOBNEW, __ASSERT_FUNCTION)
// and in libstdc++ headers. Sea-front consumes and elides it.
//
// Real-world hit: gcc 14 libcpp/makeuname2c.cc reaches sea-front
// preprocessed with '__extension__ __PRETTY_FUNCTION__' inside an
// __assert_fail call (glibc's assert macro expansion).

int main() {
    int x = __extension__ 42;
    int y = __extension__ (x + 0);
    return y;
}
