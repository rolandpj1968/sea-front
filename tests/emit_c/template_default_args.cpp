// EXPECT: 141
template<typename T, typename U = int>
struct Pair {
    T first;
    U second;
};

int main() {
    Pair<double> p;
    Pair<double, long> q;
    p.second = 42;
    q.second = 99;
    return p.second + (int)q.second;
}
