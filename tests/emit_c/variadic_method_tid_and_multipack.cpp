// EXPECT: 0
// Two related variadic gaps closed together — both visible in the
// reduced form of g++.dg/cpp0x/variadic-init.C:
//
//   1. Method-template-id with NTTP-pack args: `S<a,b,c>::foo<x,y,z>()`
//      where both heads take `int... N` packs. The member-template
//      instantiation pass's binding loops (outer-class seed AND
//      method-args from the call-site tail_tid) iterated 1:1 over
//      params×args — binding only the first arg per pack and losing
//      the rest. Result: foo<0,1,2> bound as N={0} and the cloned
//      func used Itanium `Li0E` for class args 1+ and method args 1+
//      (giving `ILi0ELi0ELi0EE` instead of `ILi0ELi1ELi2EE`).
//      Fix: when the param is_pack, fold all trailing template-args
//      into one subst_map_add_pack entry. Mirrors the main
//      instantiate_one path's behaviour. N4659 §17.5.3.
//
//   2. Multi-pack expression expansion: `(M + N)...` references two
//      packs at the same expansion site. clone_node_array_pack's
//      single_pack lookup returns NULL when multiple packs are
//      present, so the expansion fell through to a single
//      unexpanded clone. Result: `static int x[] = { (M + N), -1 }`
//      with M and N as unbound idents — link-time `M`/`N` undefined.
//      Fix: detect the multi-pack expression-expansion case, count
//      the common pack length (N4659 §17.5.3.4/2 requires all packs
//      in an expansion to share the same length), iterate that
//      many times, and per-iteration shadow EVERY pack with its
//      j-th element. Yields `{(0+0), (1+1), (2+2), -1}`.

extern "C" void abort();

template<int... M> struct S {
    template<int... N> static int sum_pairs() {
        int arr[] = { (M + N)..., -1 };
        int s = 0;
        for (unsigned i = 0; i + 1 < sizeof(arr)/sizeof(arr[0]); i++)
            s += arr[i];
        return s;
    }
};

template<typename... TS> struct R {
    template<typename... US> static int sum_sizeofs() {
        int arr[] = { (int)(sizeof(TS) + sizeof(US))..., -1 };
        int s = 0;
        for (unsigned i = 0; i + 1 < sizeof(arr)/sizeof(arr[0]); i++)
            s += arr[i];
        return s;
    }
};

int main() {
    /* S<0,1,2>::sum_pairs<10,20,30>() → (0+10)+(1+20)+(2+30) = 63 */
    if (S<0,1,2>::sum_pairs<10,20,30>() != 63) abort();
    /* Different instantiation must dedup distinctly. */
    if (S<0,1,2>::sum_pairs<0,1,2>()    != 6) abort();   /* (0+0)+(1+1)+(2+2) */

    /* Type-pack version exercises the same multi-pack path through
     * sizeof(TS)+sizeof(US) — neither pack is bound by the cloner's
     * single-pack lookup; the multi-pack fall-back zips them. */
    int e1 = (int)(sizeof(char) + sizeof(int));        /* 1+4 = 5 */
    int e2 = (int)(sizeof(short) + sizeof(double));    /* 2+8 = 10 */
    int e3 = (int)(sizeof(int) + sizeof(long));        /* 4+8 = 12 */
    if (R<char, short, int>::sum_sizeofs<int, double, long>() != e1+e2+e3) abort();

    return 0;
}
