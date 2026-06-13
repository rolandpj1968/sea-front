// EXPECT: 0
// Aggregate-init `T x{a, b, c}` on a class with no user-declared
// constructor — N4659 §11.6.1/1 [dcl.init.aggr]. Sea-front
// previously die'd in resolve_overload on "no matching overload
// for ctor"; the fix is to recognise the aggregate case and emit
// an assignment from a C99 compound literal:
//
//   struct T name;
//   name = (struct T){<args>};
//
// Pattern: g++.dg/cpp0x/initlist-nrv1.C `B ret{ A{}, "" };`.

struct S { int a; int b; int c; };

int main() {
    S s{1, 2, 3};
    return s.a + s.b + s.c - 6;
}
