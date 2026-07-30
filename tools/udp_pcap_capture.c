#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

struct pcap_file_header {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t this_zone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t link_type;
};

struct pcap_record_header {
    uint32_t timestamp_seconds;
    uint32_t timestamp_microseconds;
    uint32_t captured_length;
    uint32_t original_length;
};

static volatile sig_atomic_t should_stop;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    should_stop = 1;
}

static int packet_matches_udp_port(const uint8_t* packet, size_t length, uint16_t port)
{
    size_t offset = sizeof(struct ether_header);
    const struct ether_header* ethernet;
    const struct iphdr* ip;
    const struct udphdr* udp;
    uint16_t protocol;

    if (length < offset)
        return 0;

    ethernet = (const struct ether_header*)packet;
    protocol = ntohs(ethernet->ether_type);
    if (protocol == ETH_P_8021Q || protocol == ETH_P_8021AD) {
        if (length < offset + 4)
            return 0;
        protocol = ntohs(*(const uint16_t*)(packet + offset + 2));
        offset += 4;
    }
    if (protocol != ETH_P_IP || length < offset + sizeof(struct iphdr))
        return 0;

    ip = (const struct iphdr*)(packet + offset);
    if (ip->version != 4 || ip->protocol != IPPROTO_UDP)
        return 0;
    offset += (size_t)ip->ihl * 4;
    if (length < offset + sizeof(struct udphdr))
        return 0;

    udp = (const struct udphdr*)(packet + offset);
    return ntohs(udp->source) == port || ntohs(udp->dest) == port;
}

int main(int argc, char** argv)
{
    const char* interface_name;
    const char* output_path;
    uint16_t port;
    struct sockaddr_ll address = { 0 };
    struct pcap_file_header file_header = {
        .magic = 0xa1b2c3d4,
        .version_major = 2,
        .version_minor = 4,
        .snaplen = 65535,
        .link_type = 1,
    };
    uint8_t packet[65535];
    unsigned long captured_packets = 0;
    int socket_fd;
    FILE* output;

    if (argc != 4) {
        fprintf(stderr, "Usage: %s <interface> <udp-port> <output.pcap>\n", argv[0]);
        return 2;
    }

    interface_name = argv[1];
    output_path = argv[3];
    {
        char* end = NULL;
        unsigned long parsed_port = strtoul(argv[2], &end, 10);
        if (!end || *end || parsed_port > 65535) {
            fprintf(stderr, "Invalid UDP port: %s\n", argv[2]);
            return 2;
        }
        port = (uint16_t)parsed_port;
    }

    socket_fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (socket_fd < 0) {
        fprintf(stderr, "socket(AF_PACKET): %s\n", strerror(errno));
        return 1;
    }

    address.sll_family = AF_PACKET;
    address.sll_protocol = htons(ETH_P_ALL);
    address.sll_ifindex = (int)if_nametoindex(interface_name);
    if (!address.sll_ifindex) {
        fprintf(stderr, "Unknown interface: %s\n", interface_name);
        close(socket_fd);
        return 1;
    }
    if (bind(socket_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        fprintf(stderr, "bind(%s): %s\n", interface_name, strerror(errno));
        close(socket_fd);
        return 1;
    }
    {
        struct timeval receive_timeout = { .tv_sec = 1 };
        if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO,
                &receive_timeout, sizeof(receive_timeout)) < 0) {
            fprintf(stderr, "setsockopt(SO_RCVTIMEO): %s\n", strerror(errno));
            close(socket_fd);
            return 1;
        }
    }

    output = fopen(output_path, "wb");
    if (!output) {
        fprintf(stderr, "fopen(%s): %s\n", output_path, strerror(errno));
        close(socket_fd);
        return 1;
    }
    if (fwrite(&file_header, sizeof(file_header), 1, output) != 1) {
        fprintf(stderr, "write pcap header failed\n");
        fclose(output);
        close(socket_fd);
        return 1;
    }
    fflush(output);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    while (!should_stop) {
        struct pcap_record_header record_header;
        struct timeval now;
        ssize_t packet_length = recv(socket_fd, packet, sizeof(packet), 0);

        if (packet_length < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            fprintf(stderr, "recv: %s\n", strerror(errno));
            break;
        }
        if (port && !packet_matches_udp_port(packet, (size_t)packet_length, port))
            continue;

        gettimeofday(&now, NULL);
        record_header.timestamp_seconds = (uint32_t)now.tv_sec;
        record_header.timestamp_microseconds = (uint32_t)now.tv_usec;
        record_header.captured_length = (uint32_t)packet_length;
        record_header.original_length = (uint32_t)packet_length;
        if (fwrite(&record_header, sizeof(record_header), 1, output) != 1
            || fwrite(packet, (size_t)packet_length, 1, output) != 1) {
            fprintf(stderr, "write packet failed\n");
            break;
        }
        captured_packets++;
        fflush(output);
    }

    fprintf(stderr, "Captured %lu UDP packets on port %u\n", captured_packets, port);
    fclose(output);
    close(socket_fd);
    return 0;
}
