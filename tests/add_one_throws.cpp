// Propagating throw (no catch in this function).
extern "C" unsigned add_one(unsigned x) {
    if (x == 42u) throw 1;
    return x + 1;
}
