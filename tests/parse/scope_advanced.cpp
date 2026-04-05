typedef int Foo;
int main() {
    Foo x = 1;
    {
        int Foo = 2;
        int y = Foo;
    }
    Foo z = 3;
    for (int i = 0; i < 10; i = i + 1) {
        int x = i;
    }
    return x + z;
}
