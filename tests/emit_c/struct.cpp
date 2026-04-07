// EXPECT: 7
// First C++ struct end-to-end. Sema's visit_member walks the class
// scope (Point's class_region) to resolve x and y as int members.
// Codegen emits a C struct definition + member access via '.'.
struct Point {
    int x;
    int y;
};

int main() {
    Point p;
    p.x = 3;
    p.y = 4;
    return p.x + p.y;
}
