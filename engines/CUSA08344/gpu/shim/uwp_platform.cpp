#include "../../src/uwp_host.h"
namespace { BbUwpHost uwp_host{}; }
extern "C" int bbgpu_uwp_set_host(const BbUwpHost* host) {
    if (!host || host->size != sizeof(BbUwpHost) || host->version != 1 || !host->context ||
        !host->extent || !host->attach_swapchain || !host->read_input || !host->closed) return 87;
    uwp_host = *host;
    return 0;
}
extern "C" int bbgpu_uwp_active() { return uwp_host.context != nullptr; }
extern "C" void bbgpu_uwp_extent(uint32_t* width, uint32_t* height) {
    if (uwp_host.extent) uwp_host.extent(uwp_host.context, width, height);
}
extern "C" int bbgpu_uwp_closed() { return uwp_host.closed && uwp_host.closed(uwp_host.context); }
extern "C" int bbgpu_uwp_input(BbUwpPad* pad) {
    if (!uwp_host.read_input) return 0;
    uwp_host.read_input(uwp_host.context, pad);
    return 1;
}
