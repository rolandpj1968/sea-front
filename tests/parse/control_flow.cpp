int main() {
    int x = 10;
    while (x > 0)
        x = x - 1;
    for (int i = 0; i < 5; i = i + 1)
        x = x + i;
    do {
        x = x + 1;
    } while (x < 100);
    return x;
}
