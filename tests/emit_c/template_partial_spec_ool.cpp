// EXPECT: 42
// Partial specialization with out-of-class method definitions
struct prefix { int num; };
struct vl_embed {};
template<typename T, typename A, typename L> struct S;

template<typename T, typename A>
struct S<T, A, vl_embed> {
    prefix pfx;
    T data[8];
    int length();
    T get(int i);
    void push(T val);
};

template<typename T, typename A>
int S<T, A, vl_embed>::length() { return pfx.num; }

template<typename T, typename A>
T S<T, A, vl_embed>::get(int i) { return data[i]; }

template<typename T, typename A>
void S<T, A, vl_embed>::push(T val) {
    data[pfx.num] = val;
    pfx.num = pfx.num + 1;
}

int main() {
    S<int, int, vl_embed> s;
    s.pfx.num = 0;
    s.push(10);
    s.push(20);
    s.push(12);
    return s.get(0) + s.get(1) + s.get(2);
}
