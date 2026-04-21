// EXPECT: 1
// N4659 §6.3.10/2 [basic.scope.hiding]: "A class name or enumeration
// name can be hidden by the name of a variable, data member, function,
// or enumerator declared in the same scope."
//
// Pattern from gcc 4.8 rtl.h: an enum 'rtx_class' and a same-named
// array 'extern const enum rtx_class rtx_class[...]'. Bare 'rtx_class'
// in expression position refers to the VARIABLE (the array); to name
// the type you must use the elaborated-type-specifier 'enum rtx_class'.
//
// Sea-front's parser_at_type_specifier previously returned true for any
// IDENT registered as ENTITY_TYPE/ENTITY_TAG, ignoring the variable that
// hides it. That made '(rtx_class[i]) & 1' parse as a cast
// '(enum rtx_class *) (&1)' instead of 'rtx_class[i] & 1', producing
// bogus C like '(enum rtx_class*)(&(~2)) == ...' that the downstream
// compiler rejects with 'lvalue required as unary & operand'.
//
// Fix: when lookup finds a same-named non-function ENTITY_VARIABLE in
// scope, the tag is hidden in ordinary lookup — not a type-specifier.
enum my_tag { RTX_A, RTX_B, RTX_C };
extern const enum my_tag my_tag[3];
const enum my_tag my_tag[3] = { RTX_A, RTX_B, RTX_C };

int main() {
    int i = 1;
    // Parenthesised identifier followed by '&' used to be mis-parsed
    // as a cast to type 'enum my_tag' applied to '&(<expr>)'.
    int v = (my_tag[i]) & 1;     // my_tag[1]=RTX_B=1, 1 & 1 = 1
    return v;
}
