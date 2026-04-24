// EXPECT: 42
// Passing a scalar rvalue (arithmetic result) to a T& parameter must
// materialize the rvalue so '&' has a valid operand. The simple emit
// '&(a + b)' is illegal C because '(a + b)' is an rvalue. For scalar
// types (int, long, ptr, etc.) the C99 compound literal '(T){expr}'
// IS an lvalue with block-scoped lifetime — wrap the rvalue in one.
// N4659 §7.2.1 [basic.lval] / C11 §6.5.2.5.
//
// Pattern: gcc 4.8 cfgexpand.c expand_stack_vars
//   data->asan_vec.safe_push(offset + stack_vars[i].size);

struct Sink {
    long total;
    void add(long &v) { total += v; }
};

int main() {
    Sink s; s.total = 0;
    long a = 10, b = 12;
    s.add(a + b);        // rvalue long passed to long&
    s.add(20);           // literal rvalue passed to long&
    return (int)s.total;
}
