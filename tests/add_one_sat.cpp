// No exception handling at all. Pre-pass: SAT (add_one(x) == x + 1).
extern "C" unsigned add_one(unsigned x) {
    return x + 1;
}
