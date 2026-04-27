// EXPECT: 0
// N4659 §11/3 [dcl.dcl]: in 'decl-specifier-seq init-declarator-list',
// the decl-specifier-seq (including 'extern') applies to EVERY
// declarator in the comma-separated list. Sea-front previously only
// OR'd spec.flags into the FIRST declarator; subsequent ones — and,
// when comma-separation triggered the multi-decl path that returns
// early, the first one too — were missing storage flags. Result:
// 'extern int a, b;' emitted as 'int a; int b;', flipping both into
// tentative definitions. Multiple TUs that included the declaration
// then collided at link time with 'multiple definition' errors.
//
// Surfaced by gcc 4.8 rtl.h's
//   extern location_t prologue_location, epilogue_location;
// declared once but defined elsewhere — the missing extern made
// every TU define them, breaking the gen-tool link stage.
extern int multi_a, multi_b, multi_c;

int main() { return 0; }
