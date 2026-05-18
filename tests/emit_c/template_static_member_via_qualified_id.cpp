// EXPECT: 42
// 'ClassTmpl<T>::staticmem' is a qualified-id where the leading part
// is a template-id. Sea-front parses the lead_tid and carries it on
// the ND_QUALIFIED, but the static-data-member emit had been
// rebuilding only the bare class tag — losing the template args
// produced a mangled symbol that didn't match the instantiation's
// definition. N4659 §17.7.1 [temp.inst] requires the access to
// resolve to the specific instantiation.
//
// Emit now consults qualified.lead_tid and rebuilds the stub Type
// with template_args populated, so mangle_class_tag produces
// 'sf__Wrap_t_A_te___v' rather than 'sf__Wrap__v'.

struct A { int x; };
union U { int u; };

template<typename T>
struct Wrap { static const int v = __is_class(T) ? 1 : 0; };

int main() {
    int r1 = Wrap<A>::v;    /* expect 1 */
    int r2 = Wrap<U>::v;    /* expect 0 */
    int r3 = Wrap<int>::v;  /* expect 0 */
    return (r1 == 1 && r2 == 0 && r3 == 0) ? 42 : 1;
}
