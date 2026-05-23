// EXPECT: 0
// 'delete p' on a polymorphic base pointer routes through the
// vtable's __dtor slot. Sea-front emits
//   _ZdlPv(((struct Root *)(p))->__sf_vptr->__dtor(p))
// where the vptr-load cast goes to the polymorphic root type, but
// the __dtor arg 'p' was left as the source type. The slot's
// declared first param is 'struct Root *' too, so the arg needs
// the same cast — strict-C back-ends (cproc) reject the unconverted
// arg as a pointer base-type mismatch.
//
// Fix: cast both the vptr-load receiver AND the __dtor call arg
// to the root type. gcc accepted the unconverted arg via silent
// derived-to-base conversion.

struct B { virtual ~B() {} };
struct D : B { int x; D() : x(0) {} };

int main() {
    B *p = new D;
    delete p;
    return 0;
}
