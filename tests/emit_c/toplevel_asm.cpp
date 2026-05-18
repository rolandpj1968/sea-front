// EXPECT: 0
// asm-declaration at namespace scope — N4659 §10.4 [dcl.asm]
//   asm ( string-literal ) ;
// libstdc++ 13 <iostream> uses '__extension__ __asm(".globl ...");'
// at namespace scope; without the parse arm, every test that
// transitively includes <iostream> failed at the parser.
//
// Sea-front skips the directive entirely — it's a backend hint with
// no semantic effect on the emitted C.

namespace N {
    __extension__ __asm (".globl __sf_toplevel_asm_marker");
}

int main() { return 0; }
