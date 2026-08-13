// Same simple name as named_namespace_b.cpp's Gadget, but each is inside a
// different named namespace, so the linker symbols are namespace-qualified
// and cannot collide.
namespace lintfixture_a {
struct Gadget {
    int x = 0;
};
}  // namespace lintfixture_a
