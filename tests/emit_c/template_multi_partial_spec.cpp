// EXPECT: 42
// Out-of-class method definitions of partial specializations must bind
// only to instantiations of THAT specialization, not to every
// instantiation sharing the class tag. Prior to per-spec OOL binding,
// vl_embed's OOL methods were also attached to vl_ptr instantiations
// (and vice versa), causing 'member undeclared' C errors.
struct vl_embed { };
struct vl_ptr   { };

template<typename T, typename A, typename L> struct vec;

struct prefix { int num_; };

template<typename T, typename A>
struct vec<T, A, vl_embed> {
    prefix pfx_;
    int get_embed();
};

template<typename T, typename A>
struct vec<T, A, vl_ptr> {
    int direct_;
    int get_ptr();
};

template<typename T, typename A>
int vec<T, A, vl_embed>::get_embed() { return pfx_.num_; }

template<typename T, typename A>
int vec<T, A, vl_ptr>::get_ptr() { return direct_; }

struct alloc { };

int main() {
    vec<int, alloc, vl_embed> ve;
    ve.pfx_.num_ = 20;

    vec<int, alloc, vl_ptr> vp;
    vp.direct_ = 22;

    return ve.get_embed() + vp.get_ptr();
}
