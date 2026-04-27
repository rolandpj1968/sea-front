// EXPECT: 0
// N4659 §11.4.9 [class.static] — a static member function calling
// an unqualified sibling static method must NOT get an implicit
// 'this->' (static methods have no this) AND must still resolve
// to the class-mangled symbol (sf__C__sibling_p_..._pe_).
//
// The first half (#136) was already fixed for in-class definitions.
// This test covers the OOL definition case: 'void C::caller(...)
// { sibling(...); }' where 'static' is implied by the in-class
// declaration. Sea-front propagates DECL_STATIC from the in-class
// decl to the OOL func, then emit suppresses the 'this->' prefix
// when emitting inside a static method body.
//
// Member-template variant (the gcc 4.8 vec.h va_heap::release case)
// is partially fixed — the same suppression + mangle plumbing
// works, but the unqualified-call instantiation trigger is a
// separate gap (filed as #145).
struct C {
    static int sibling(int x);
    static int caller(int x);
};

int C::sibling(int x) { return x + 1; }

int C::caller(int x) {
    return sibling(x);   // unqualified — must not get this->
}

int main() { return C::caller(-1); }
