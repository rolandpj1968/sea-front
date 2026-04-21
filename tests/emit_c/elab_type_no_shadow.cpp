// EXPECT: 42
// A later bodyless elaborated-type-specifier ('struct Foo *') must
// NOT shadow the earlier COMPLETE definition in the tag registry.
// Before the fix, 'typedef struct SetDef *Set;' after
// 'struct SetDef { ... };' prepended a second Type (with no
// class_region) to the ENTITY_TAG bucket; later lookups walked
// head-first and returned that bodyless Type, so member access
// couldn't resolve through it.
//
// N4659 §10.1.7.3 [dcl.type.elab]: an elaborated-type-specifier
// referring to an already-declared name is a REFERENCE to the
// existing type, not a new declaration.
struct SetDef {
    int val;
};
typedef struct SetDef *Set;   // must NOT overwrite the complete SetDef

int main() {
    SetDef s;
    s.val = 42;
    Set p = &s;
    return p->val;   // access through typedef — requires SetDef's region
}
