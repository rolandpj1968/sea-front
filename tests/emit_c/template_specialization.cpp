// EXPECT: 12
template<typename T>
struct TypeInfo {
    int size() { return 0; }
};

template<>
struct TypeInfo<int> {
    int size() { return 4; }
};

template<>
struct TypeInfo<double> {
    int size() { return 8; }
};

int main() {
    TypeInfo<int> ti;
    TypeInfo<double> td;
    TypeInfo<char> tc;
    return ti.size() + td.size() + tc.size();
}
