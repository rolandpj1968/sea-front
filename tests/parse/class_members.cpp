struct Point {
    int x;
    int y;
    int sum() { return x + y; }
};

class Animal {
public:
    int age;
    void speak();
private:
    int internal;
protected:
    int shared;
};

template<typename T>
struct Container {
    T value;
    T get() { return value; }
};

Point p;
Container<int> c;
int main() { p.x = 1; return p.sum(); }
