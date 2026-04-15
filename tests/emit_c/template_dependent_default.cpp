// EXPECT: 42
// Template default argument with dependent qualified name:
// 'typename A::default_layout' must resolve to the nested member of
// A once A is bound at instantiation. Without resolution, the literal
// string 'default_layout' leaked into the mangle and broke matching.
struct vl_ptr   { };
struct vl_embed { };
struct va_heap  { typedef vl_ptr default_layout; };

template<typename T, typename A, typename L> struct vec;

template<typename T,
         typename A = va_heap,
         typename L = typename A::default_layout>
struct vec { int primary_unused; };

template<typename T, typename A>
struct vec<T, A, vl_ptr>   { int ptr_val_;   int get(); };

template<typename T, typename A>
struct vec<T, A, vl_embed> { int embed_val_; int get(); };

template<typename T, typename A>
int vec<T, A, vl_ptr>::get()   { return ptr_val_; }

template<typename T, typename A>
int vec<T, A, vl_embed>::get() { return embed_val_; }

int main() {
    // vec<int> -> defaults expand to vec<int, va_heap, vl_ptr>
    // -> matches the vl_ptr partial spec, NOT the primary.
    vec<int> v;
    v.ptr_val_ = 42;
    return v.get();
}
