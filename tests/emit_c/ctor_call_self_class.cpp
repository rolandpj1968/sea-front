// EXPECT: 0
// Self-class value-construction inside the same class body — N4659
// §8.2.3 [expr.type.conv]: 'T(args)' is an explicit type conversion
// that constructs a temporary of type T. Sea-front lowers this as
// a stack-allocated temp + ctor call, then references the temp.
//
// The callee 'T' is looked up by sema. Inside T's own class scope
// the lookup can resolve to one of T's ctors (entity=VARIABLE,
// return-type void) rather than the class tag, especially when
// multiple ctors exist (default + move + deleted-copy + private
// 2-arg). The codegen ND_CALL implicit-this branch then aborts
// with "no matching overload for method on class T" because it's
// looking for a 2-arg METHOD named T (not a CTOR). The hoist-temp
// path now recognises self-class construction by callee-name ==
// enclosing-class-tag and routes accordingly.
//
// Real-world hit: gcc 14 libcpp/include/rich-location.h's
// label_text class — static factory methods 'borrow' and 'take'
// each call 'label_text(buf, owned)' on the private 2-arg ctor.

struct Lab {
    char *buf;
    bool owned;

    Lab() : buf(0), owned(false) {}
    Lab(const Lab &) = delete;
    Lab(Lab &&other) : buf(other.buf), owned(other.owned) {}
    Lab(char *b, bool o) : buf(b), owned(o) {}

    static Lab borrow(const char *p) {
        return Lab(const_cast<char *>(p), false);
    }
};

int main() {
    Lab l = Lab::borrow("x");
    return l.owned ? 1 : 0;   // 0 — borrowed, not owned
}
