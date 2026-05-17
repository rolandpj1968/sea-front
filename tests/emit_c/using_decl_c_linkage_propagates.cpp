// EXPECT: 42
// 'using ::name' inside a namespace must inherit linkage attrs
// (c_linkage / asm_name) from the source declaration. Without
// this, calls like 'std::exit(0)' resolved through 'using ::exit'
// were mangled with the namespace prefix (Itanium _ZN3std4exitEi
// or the human equivalent) — no libc symbol matched, so the
// program failed to link.
//
// Test shape: a fake extern "C" function in global scope, then
// 'using ::name' inside a namespace, then a qualified call
// resolves through the using-decl and must emit the bare/asm
// form rather than namespace-mangled.

extern "C" int sf_test_extern_c_func(int x);

namespace ns_alias {
    using ::sf_test_extern_c_func;
}

int sf_test_extern_c_func(int x) { return x; }

int main() {
    return ns_alias::sf_test_extern_c_func(42);
}
