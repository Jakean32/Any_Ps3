#include <cstdio>
#include "core/bytesource.h"
#include "core/iso9660.h"
using namespace anyps3;
int main() {
    auto src = FileSource::open("/home/z/my-project/fixtures/game.iso", nullptr);
    printf("src: %p size=%llu\n", (void*)src.get(), (unsigned long long)(src?src->size():0));
    std::string err;
    auto iso = Iso9660::open(src, &err);
    printf("iso: %p err='%s'\n", (void*)iso.get(), err.c_str());
    if (iso) {
        printf("vol=%s\n", iso->volume_id().c_str());
        auto names = iso->list_dir("PS3_GAME");
        for (auto& n : names) printf("  %s\n", n.c_str());
    }
    return 0;
}
