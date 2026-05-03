// EXPECT: 42
// Qualified static-method call where the named class is a TEMPLATE
// instantiation that inherits the static from a template base.
// Pattern from gcc 4.8 hash-table.h:
//   pointer_hash<T> : typed_noop_remove<T>
// pointer_hash<T> doesn't define remove(); it inherits the no-op
// static remove from typed_noop_remove<T>. hash_table::dispose calls
// Descriptor::remove(p) where Descriptor is bound to a pointer_hash
// instantiation — sea-front mangles the call with the derived class's
// tag, but the only def is on the BASE class.
//
// The earlier base-class walk (commit 26b9fce / task #154) only fired
// when the qualified call had no resolved_class_type; for template-id
// qualifiers sema sets it, so the walk was skipped and the call
// emitted as 'sf__pointer_hash_t_X_te___remove_*' with no def.
//
// Standard: N4659 §6.4.5 [class.qual] + §13.1 [class.derived].

template<typename T>
struct NoopRemove {
    static int remove(T* p) { return 42; }
};

template<typename T>
struct PtrHash : NoopRemove<T> {
    typedef T value_type;
};

template<typename Descriptor>
struct Container {
    typedef typename Descriptor::value_type V;
    int do_remove(V* p) {
        return Descriptor::remove(p);
    }
};

struct Item {};

int main() {
    Container<PtrHash<Item> > c;
    Item it;
    return c.do_remove(&it);
}
