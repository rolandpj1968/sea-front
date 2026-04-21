// EXPECT: 42
// '((T)expr)(args)' — calling a C-style cast result. Source uses
// this idiom to reinterpret a generic function-pointer slot as a
// specific function-pointer type, then call it. Without outer parens
// around the cast in the generated C, C precedence parses
// '(T)expr(args)' as '(T)(expr(args))' — casting the CALL result
// rather than calling the cast result. Pattern from gcc 4.8 recog.h
// which stores '(f0)func' and casts through '((fN)func)(args)' to
// dispatch N-arg variants.
// N4659 §8.2.2 [expr.call].

typedef int (*f1)(int);

int add_one(int x) { return x + 1; }

int main() {
    void* func = (void*)add_one;
    return ((f1)func)(41);  // must call add_one(41) = 42
}
