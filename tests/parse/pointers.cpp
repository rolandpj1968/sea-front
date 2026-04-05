int main() {
    int x = 42;
    int *p = &x;
    *p = *p + 1;
    int **pp = &p;
    return **pp;
}
