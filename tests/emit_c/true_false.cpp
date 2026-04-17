// EXPECT: 1
// Test: true/false emit as 1/0 in C
int main() {
    bool a = true;
    bool b = false;
    if (a && !b)
        return 1;
    return 0;
}
