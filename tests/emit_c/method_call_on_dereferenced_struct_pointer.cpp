// EXPECT: 42
// '(*p).method()' — dereferenced struct pointer as method receiver.
// The receiver is an lvalue ('*p'); '&(*p) == p' so the implicit-this
// arg can be passed directly. Sea-front previously force-hoisted the
// dereferenced struct into a stack temp and passed '&__SF_temp_N',
// which copies the struct. For containers with flexible-array-style
// trailing data (gcc 4.8 vec<T,A,vl_embed>), the copy truncates and
// reads past valid memory. N4659 §8.2.5 [expr.ref] / §8.3.1
// [expr.unary.op].
//
// Pattern: gcc 4.8 cp/name-lookup.c
//   FOR_EACH_VEC_ELT (*level->class_shadowed, i, cb)
// where class_shadowed is vec<...,va_gc,vl_embed>*; the macro expands
// to '(*level->class_shadowed).iterate(i, &cb)'. The struct-copy
// truncated the elements array, the iterate body read garbage,
// cc1plus crashed in poplevel_class on the trivial 'struct {};'.

struct Counter {
    int data[4];
    int n;
    int sum_first(unsigned upto) const {
        int s = 0;
        for (unsigned i = 0; i < upto; i++) s += data[i];
        return s;
    }
};

int main() {
    Counter c;
    c.n = 4;
    c.data[0] = 9;
    c.data[1] = 11;
    c.data[2] = 10;
    c.data[3] = 12;
    Counter *p = &c;
    return (*p).sum_first(4);   // 9+11+10+12 = 42
}
