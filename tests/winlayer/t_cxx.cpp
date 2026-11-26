/* C++ exceptions (MinGW's, through Windows' unwinding): a throw three calls deep runs
 * each frame's destructors and is caught by its type; an int is caught as one. */
#include <cstdio>
#include <stdexcept>
#include <string>
struct Guard { const char *n; ~Guard() { std::printf("destroyed %s\n", n); } };
static int depth(int n) { Guard g{"inner"}; if (n == 0) throw std::runtime_error("boom"); return depth(n - 1) + 1; }
int main() {
    try { depth(3); } catch (const std::exception &e) { std::printf("caught: %s\n", e.what()); }
    try { throw 42; } catch (int v) { std::printf("caught int %d\n", v); }
    std::printf("done\n");
    return 0;
}
