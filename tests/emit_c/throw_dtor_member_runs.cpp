// EXPECT: 0
// EH: when a destructor throws while running as part of a CL_VAR
// cleanup inside a try-block, the in-flight exception must dispatch
// to the try's handler — not the function epilogue. Equivalently:
// after running the user dtor (which threw), member subobject dtors
// still run, and then __SF_CHAIN_THROW transfers to the handler.
//
// Regression for g++.dg/eh/ctor1.C: the cleanup chain was emitting
// __SF_CHAIN_ANY(__SF_epilogue) which collapsed THROW with RETURN
// and bypassed the enclosing catch. Fix emits __SF_CHAIN_THROW to
// the handler before the RETURN chain.
//
// N4659 §15.2 [class.dtor]/3 — the destructor of every direct
// non-variant non-static data member runs even when the body of the
// enclosing destructor exited via an exception.

bool foo_dtor_ran = false;

struct Foo {
    ~Foo() { foo_dtor_ran = true; }
};

struct Bar {
    ~Bar() { throw 1; }
    Foo f;
};

int main() {
    try {
        Bar b;
    } catch (int i) {
        if (i == 1 && foo_dtor_ran) return 0;
    }
    return 1;
}
