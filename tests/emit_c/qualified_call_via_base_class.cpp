// EXPECT: 5
// gcc 4.8 hash_table::dispose body calls Descriptor::remove(entries[i])
// where Descriptor (e.g. asan_mem_ref_hasher) inherits remove() from
// typed_noop_remove<T>. C++ resolves Descriptor::remove via base-
// class lookup; the def is mangled with the BASE class's tag.
// Sea-front previously emitted sf__<Descriptor>__remove_* — but the
// only def was sf__typed_noop_remove_t_..._te___remove_*. Link
// failed with ~22 distinct undefined refs.
//
// Fix: at the qualified-call codegen, when the named class doesn't
// directly own the method but a base class does, mangle through
// the base class's owner_type. Requires the class_region's bases
// to be linked even for plain (non-template) classes inheriting
// from a template instantiation — patch_all_types now also walks
// top-level ND_CLASS_DEF nodes (not just instantiated ones).
//
// Standard: N4659 §6.4.5 [class.qual] (qualified name lookup in a
// class scope walks bases) + §13.1 [class.derived].

template<typename T>
struct typed_noop_remove {
    static int remove(T *) { return 0; }
};

struct asan_hasher : typed_noop_remove<int> {
    typedef int value_type;
};

template<typename Desc>
struct Container {
    int run() {
        int x = 5;
        Desc::remove(&x);   // resolves to typed_noop_remove<int>::remove
        return 5;
    }
};

int main() {
    Container<asan_hasher> c;
    return c.run();
}
