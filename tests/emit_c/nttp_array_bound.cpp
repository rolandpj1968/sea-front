// EXPECT: 5
// Non-type template parameter as array bound — N4659 §11.3.4/1
// [dcl.array]: the array bound is a constant-expression. When the
// bound names a non-type template parameter, clone.c's subst_type
// must substitute the NTTP value into the array's size expression
// — otherwise the cloned class body emits 'T m_buf[NUM]' with NUM
// undeclared at TU scope.
//
// Real-world hit: gcc 14 diagnostic-show-locus.cc declares
// 'template<class T, int NUM_EMBEDDED> class semi_embedded_vec'
// with 'T m_embedded[NUM_EMBEDDED];'. Without this substitution,
// every libcpp source compiling diagnostic-show-locus.h fails at
// cc with 'NUM_EMBEDDED undeclared here'.

template <typename T, int NUM>
struct EmbVec {
    T data[NUM];
};

int main() {
    EmbVec<int, 5> v;
    int count = 0;
    for (int i = 0; i < 5; ++i) {
        v.data[i] = i;
        count++;
    }
    return count;
}
