// EXPECT: 15
// Anonymous nested struct/union members — the libcpp internal.h
// pattern. Exercises stable anon_id naming so the definition and
// every reference use the same __sf_anon_N name.
struct token {
    int type;
    union {
        struct {
            int node_type;
            int flags;
        } u;
        struct {
            int value;
        } c;
    } data;
};

int main() {
    struct token t;
    t.type = 1;
    t.data.u.node_type = 5;
    t.data.u.flags = 10;
    return t.data.u.node_type + t.data.u.flags;
}
