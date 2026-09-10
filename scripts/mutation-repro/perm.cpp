#include <string>
static bool overLimit(int n) { return n > 5; }                       // cxx_gt_to_ge
static bool isReg(const std::string& k) { return k == "register"; }  // cxx_replace_scalar_call
static int addTwo(int a, int b) { return a + b; }                    // cxx_add_to_sub

int main() {
    bool flag = false;          // cxx_init_const: -> true
    if (flag) { return 1; }

    bool assigned = true;
    assigned = false;           // cxx_assign_const: -> true
    if (assigned) { return 2; }

    if (overLimit(5)) { return 3; }
    if (isReg("nope")) { return 4; }
    if (addTwo(2, 2) != 4) { return 5; }
    return 0;
}
