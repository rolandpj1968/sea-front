// Template instantiation: basic class template with type substitution
template<typename T>
struct Box {
    T val;
};

template<typename A, typename B>
struct Pair {
    A first;
    B second;
};

Box<int> b;
Pair<int, double> p;
