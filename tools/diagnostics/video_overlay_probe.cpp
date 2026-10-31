#include "../../wpe-drm/VideoOverlayWire.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct Buffer {
    uint32_t handle { 0 };
    uint32_t pitch { 0 };
    uint64_t size { 0 };
    void* pixels { nullptr };
    int fd { -1 };
};

static bool createBuffer(int drm, Buffer& buffer, unsigned luminance)
{
    drm_mode_create_dumb create { };
    create.width = 160;
    create.height = 144;
    create.bpp = 8;
    if (ioctl(drm, DRM_IOCTL_MODE_CREATE_DUMB, &create))
        return false;
    buffer.handle = create.handle;
    buffer.pitch = create.pitch;
    buffer.size = create.size;
    drm_mode_map_dumb map { };
    map.handle = buffer.handle;
    if (ioctl(drm, DRM_IOCTL_MODE_MAP_DUMB, &map))
        return false;
    buffer.pixels = mmap(nullptr, buffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm, map.offset);
    if (buffer.pixels == MAP_FAILED) {
        buffer.pixels = nullptr;
        return false;
    }
    memset(buffer.pixels, luminance, static_cast<size_t>(buffer.pitch) * 96);
    memset(static_cast<uint8_t*>(buffer.pixels) + static_cast<size_t>(buffer.pitch) * 96,
        128, static_cast<size_t>(buffer.pitch) * 48);
    return !drmPrimeHandleToFD(drm, buffer.handle, DRM_CLOEXEC | DRM_RDWR, &buffer.fd);
}

static bool sendFrame(int socket, const Buffer& buffer, uint64_t sequence)
{
    VideoOverlayWireMessage frame;
    frame.type = VideoOverlayMessageFrame;
    frame.sequence = sequence;
    frame.fourcc = 0x3231564e;
    frame.width = 160;
    frame.height = 96;
    frame.planeCount = 2;
    frame.strides[0] = frame.strides[1] = buffer.pitch;
    frame.offsets[1] = buffer.pitch * frame.height;
    frame.rectX = 80;
    frame.rectY = 60;
    frame.rectWidth = 160;
    frame.rectHeight = 96;
    iovec vector { &frame, sizeof(frame) };
    char control[CMSG_SPACE(sizeof(int))] { };
    msghdr message { };
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &buffer.fd, sizeof(buffer.fd));
    return sendmsg(socket, &message, MSG_NOSIGNAL) == sizeof(frame);
}

static bool receive(int socket, unsigned timeout, uint32_t type, uint64_t sequence)
{
    pollfd pollFD { socket, POLLIN, 0 };
    if (poll(&pollFD, 1, timeout) <= 0)
        return false;
    VideoOverlayWireMessage message;
    ssize_t count = recv(socket, &message, sizeof(message), 0);
    if (!videoOverlayWireValid(message, count) || message.type != type || message.sequence != sequence) {
        fprintf(stderr, "Unexpected ACK type=%u sequence=%llu\n", message.type,
            static_cast<unsigned long long>(message.sequence));
        return false;
    }
    printf("ACK type=%u sequence=%llu\n", type, static_cast<unsigned long long>(sequence));
    return true;
}

int main(int argc, char** argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s /dev/dri/card0 SOCKET\n", argv[0]);
        return 2;
    }
    int drm = open(argv[1], O_RDWR | O_CLOEXEC);
    int channel = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    sockaddr_un address { };
    address.sun_family = AF_UNIX;
    if (drm < 0 || channel < 0 || strlen(argv[2]) >= sizeof(address.sun_path))
        return 1;
    strcpy(address.sun_path, argv[2]);
    if (connect(channel, reinterpret_cast<sockaddr*>(&address), sizeof(address)))
        return 1;
    Buffer buffers[2];
    if (!createBuffer(drm, buffers[0], 70) || !createBuffer(drm, buffers[1], 190)) {
        perror("NV12 dumb buffer");
        return 1;
    }
    if (!sendFrame(channel, buffers[0], 1))
        return 1;
    pollfd early { channel, POLLIN, 0 };
    if (poll(&early, 1, 350) > 0) {
        fprintf(stderr, "Current scanout was released before replacement\n");
        return 1;
    }
    if (!sendFrame(channel, buffers[1], 2)
        || !receive(channel, 2000, VideoOverlayMessageRelease, 1))
        return 1;
    VideoOverlayWireMessage hide;
    hide.type = VideoOverlayMessageHide;
    hide.sequence = 3;
    if (send(channel, &hide, sizeof(hide), MSG_NOSIGNAL) != sizeof(hide)
        || !receive(channel, 2000, VideoOverlayMessageRelease, 2)
        || !receive(channel, 2000, VideoOverlayMessageCloseComplete, 3))
        return 1;
    close(channel);
    for (auto& buffer : buffers) {
        close(buffer.fd);
        munmap(buffer.pixels, buffer.size);
        drm_mode_destroy_dumb destroy { buffer.handle };
        ioctl(drm, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
    close(drm);
    puts("PASS video v3: current held, replacement released, hide acknowledged after detach");
    return 0;
}
