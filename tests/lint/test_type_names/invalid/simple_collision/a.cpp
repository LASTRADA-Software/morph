// Minimal reproduction of issue #84: two file-scope struct declarations
// with the same name, no enclosing namespace, in different files.
struct OrderModel {
    int x = 0;
};
