// Any_Ps3 core test suite entry point.
#include "harness.h"

namespace any_ps3 {
const TestCase* util_tests();      size_t util_tests_count();
const TestCase* sfo_tests();       size_t sfo_tests_count();
const TestCase* iso_tests();       size_t iso_tests_count();
const TestCase* elf_tests();       size_t elf_tests_count();
const TestCase* pipeline_tests();  size_t pipeline_tests_count();
}

int main() {
    using namespace any_ps3;
    std::printf("Any_Ps3 core test suite\n========================\n");

    int total_fail = 0;
    std::printf("\n[util / json]\n");
    total_fail += run_tests(util_tests(), util_tests_count());
    std::printf("\n[param.sfo]\n");
    total_fail += run_tests(sfo_tests(), sfo_tests_count());
    std::printf("\n[iso9660 via fd]\n");
    total_fail += run_tests(iso_tests(), iso_tests_count());
    std::printf("\n[elf / ppu / spu]\n");
    total_fail += run_tests(elf_tests(), elf_tests_count());
    std::printf("\n[interp / lv2 / manifest / pipeline]\n");
    total_fail += run_tests(pipeline_tests(), pipeline_tests_count());

    std::printf("\n%s\n", total_fail == 0 ? "ALL TESTS PASSED" : "FAILURES PRESENT");
    return total_fail == 0 ? 0 : 1;
}
