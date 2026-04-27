// EXPECT: 7
// A free function that:
//   (a) takes a class-typed parameter (so the class-param mangling
//       heuristic kicks in — sea-front emits a mangled symbol name), AND
//   (b) returns a function pointer.
//
// Sea-front's emit_free_func_header was emitting the mangled form as
//   <ret-type> <mangled-name>(<params>);
// which is invalid C when <ret-type> is a function pointer:
//   _Bool (*)(union tree_node *) name(...)
// is a standalone function-pointer type followed by an identifier — cc
// errors "expected identifier or '(' before ')'". The standard C
// declarator form for a function returning a function pointer
// interleaves the name inside the pointer:
//   _Bool (*name(...))(union tree_node *)
// emit_func_header (the unmangled path) handles this; the mangled
// path didn't, so any class-param-taking gcc function returning a
// function pointer (e.g. gimplify.c's 'gimple_predicate
// rhs_predicate_for(tree lhs)' which returns 'bool (*)(tree)') failed
// to compile through sea-front-cc.
//
// Pattern: gcc 4.8 gimplify.c rhs_predicate_for. N4659 §11.3
// [dcl.meaning] / C99 §6.7.5.3 [Function declarators].
struct Tag { int x; };

typedef int (*pred)(int);

static int multiply_by_seven(int x) { return x * 7; }

// Takes a class-typed param (so the mangling heuristic fires) AND
// returns a function pointer.
pred get_predicate(struct Tag *t) {
    (void)t;
    return &multiply_by_seven;
}

int main() {
    struct Tag t;
    pred f = get_predicate(&t);
    return f(1);   // 7
}
