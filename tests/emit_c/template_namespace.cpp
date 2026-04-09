// EXPECT: 42
namespace ns {
    template<typename T>
    struct Box {
        T val;
        T get() { return val; }
    };
}

int main() {
    ns::Box<int> b;
    b.val = 42;
    return b.get();
}
