// EXPECT: 42
// Diamond inheritance with explicit transitive-base mem-init —
// N4659 §15.6.2 [class.base.init]. 'Diamond : SubA, SubB' where
// both SubA and SubB inherit (virtually) from Base; Diamond's
// ctor names 'Base(text)' in its mem-init list. Standard says
// only the most-derived class's ctor initialises a virtual base
// (once).
//
// Sea-front doesn't model the most-derived/intermediate split,
// so each path's Base subobject is independently initialised by
// the intermediate ctor. To make the user's explicit mem-init
// have the observable effect, run it AFTER the direct sub
// ctors as a re-initialisation on the first base-path the
// transitive-base name resolves through. Member access in sea-
// front also resolves through the same first path, so the read
// sees the user's value.
//
// Test pattern modelled on gcc 4.8 g++.dg/init/vbase1.

struct Base {
    const int text;
    Base() : text(1) {}
    Base(int t) : text(t) {}
};

struct SubA : public virtual Base {
    int x;
    SubA(int ax) : x(ax) {}
};

struct SubB : public virtual Base {
    SubB() {}
};

struct Diamond : public SubA, public SubB {
    Diamond(int t) : Base(t), SubA(5), SubB() {}
};

int main() {
    Diamond d(42);
    return d.text;       // should be 42, not the default 1
}
