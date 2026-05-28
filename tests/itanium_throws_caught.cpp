// Itanium sanity test: no stdlib headers, single catch.
extern "C" int sink(int);

extern "C" unsigned add_one(unsigned x) {
    try {
        if (x == 42u) throw 1;
    } catch (int e) {
        sink(e);
    }
    return x + 1;
}
