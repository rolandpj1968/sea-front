// EXPECT: 5
// Pointer to local: address-of and dereference.
// Sema: visit_unary &x produces TY_PTR(int); *p produces TY_INT.
int main() {
    int x = 5;
    int *p = &x;
    return *p;
}
