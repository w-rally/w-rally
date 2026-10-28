// A library linked whole with only the ones below it (tests/CMakeLists.txt physics-layer-*): it links only if nothing in it
// calls into a library above it. Nothing runs.
int main() { return 0; }
