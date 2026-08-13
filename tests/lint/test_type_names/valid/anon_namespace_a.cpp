// Same name as anon_namespace_b.cpp's Widget, but both are confined to an
// anonymous namespace -- internal linkage, never visible to another
// translation unit, so this is not an ODR risk.
namespace {
struct Widget {
    int x = 0;
};
}  // namespace
