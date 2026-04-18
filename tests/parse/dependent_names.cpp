// Test: dependent qualified names parse as calls, not declarations
struct Alloc {
    template<typename T> static void release(T*& p);
};
template<typename T, typename A>
struct Box {
    T *data;
    void cleanup() { if (data) A::release(data); }
};
