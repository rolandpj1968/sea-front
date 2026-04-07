// EXPECT: 24
// Factorial of 4 via for loop.
int main() {
    int result = 1;
    for (int i = 1; i <= 4; i = i + 1) {
        result = result * i;
    }
    return result;
}
