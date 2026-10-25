#include "anyps3/audio/audio.h"

namespace anyps3 {

class NullAudioBackend : public AudioBackend {
public:
    std::string name() const override { return "null"; }
    bool Initialize() override { return true; }
    void Shutdown() override {}
};

AudioBackend* CreateNullAudioBackend() { return new NullAudioBackend(); }

}  // namespace anyps3
