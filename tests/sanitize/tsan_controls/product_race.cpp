// Race-positive control: two unsynchronised writers of an int and of a std::string. Both races MUST still be reported with the
// project's tsan.supp loaded, so the Qt-internal suppressions cannot be hiding races in Hyprnotes-style code (plain fields, std types).
#include <cstdio>
#include <string>
#include <thread>

int counter = 0;
std::string name;

int main() {
    std::thread a([] { for (int i = 0; i < 100000; ++i) { ++counter; name = std::to_string(i); } });
    std::thread b([] { for (int i = 0; i < 100000; ++i) { ++counter; name = std::to_string(-i); } });
    a.join();
    b.join();
    std::printf("%d %s\n", counter, name.c_str());
    return 0;
}
