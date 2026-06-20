// EXPECT: 0
// `sizeof(Box<Ts>)...` for `<int, char>`: clone_node_array_pack's
// per-iteration type-pack shadow (commit 8eae806) correctly produces
// `Box<int>` and `Box<char>` template-ids in the cloned body, but
// the dep-collector never reached the embedded template-id because
// ND_SIZEOF wasn't in collect_from_node's switch. Both Box<int> and
// Box<char> stayed forward-declared and the C compiler rejected
// `sizeof(struct sf__Box_t_int_te_)` as incomplete type.
//
// Adding ND_SIZEOF (+ ND_ALIGNOF / ND_OFFSETOF for the same reason)
// to collect_from_node closes the discovery loop. N4659 §17.7.1
// [temp.inst]: an implicit instantiation covers any specialization
// referenced by the program — `Box<int>` referenced via sizeof
// counts.

extern "C" void abort();

template<typename T> struct Box { T val; };

template<typename... Ts> struct Boxen {
    static int sum_sizeofs() {
        int sizes[] = { (int)sizeof(Box<Ts>)... };
        int s = 0;
        for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++)
            s += sizes[i];
        return s;
    }
};

int main() {
    int expected = (int)(sizeof(Box<int>) + sizeof(Box<char>));
    if (Boxen<int, char>::sum_sizeofs() != expected) abort();
    return 0;
}
