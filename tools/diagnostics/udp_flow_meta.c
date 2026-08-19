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
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static volatile sig_atomic_t should_stop;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    should_stop = 1;
}

static int parse_udp_header(const uint8_t *packet, size_t length,
    const struct iphdr **ip_out, const struct udphdr **udp_out)
{
    size_t offset = sizeof(struct ether_header);
    uint16_t protocol;

    if (length < offset)
        return 0;

    const struct ether_header *ethernet = (const struct ether_header *)packet;
    protocol = ntohs(ethernet->ether_type);
    if (protocol == ETH_P_8021Q || protocol == ETH_P_8021AD) {
        if (length < offset + 4)
            return 0;
        protocol = ntohs(*(const uint16_t *)(packet + offset + 2));
        offset += 4;
    }
    if (protocol != ETH_P_IP || length < offset + sizeof(struct iphdr))
        return 0;

    const struct iphdr *ip = (const struct iphdr *)(packet + offset);
    size_t ip_header_length = (size_t)ip->ihl * 4;
    if (ip->version != 4 || ip->protocol != IPPROTO_UDP
        || ip_header_length < sizeof(struct iphdr)
        || length < offset + ip_header_length + sizeof(struct udphdr))
        return 0;

    *ip_out = ip;
    *udp_out = (const struct udphdr *)(packet + offset + ip_header_length);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <interface> <output.tsv>\n", argv[0]);
        return 2;
    }

    int socket_fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (socket_fd < 0) {
        fprintf(stderr, "socket(AF_PACKET): %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_ll address = {
        .sll_family = AF_PACKET,
        .sll_protocol = htons(ETH_P_ALL),
        .sll_ifindex = (int)if_nametoindex(argv[1]),
    };
    if (!address.sll_ifindex) {
        fprintf(stderr, "Unknown interface: %s\n", argv[1]);
        close(socket_fd);
        return 1;
    }
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        fprintf(stderr, "bind(%s): %s\n", argv[1], strerror(errno));
        close(socket_fd);
        return 1;
    }

    FILE *output = fopen(argv[2], "w");
    if (!output) {
        fprintf(stderr, "fopen(%s): %s\n", argv[2], strerror(errno));
        close(socket_fd);
        return 1;
    }
    chmod(argv[2], 0600);
    fputs("seconds\tmicroseconds\tsource_ip\tsource_port\tdestination_ip\tdestination_port\tudp_bytes\n", output);
    fflush(output);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    unsigned long records = 0;
    uint8_t packet[65535];
    while (!should_stop) {
        ssize_t packet_length = recv(socket_fd, packet, sizeof(packet), 0);
        if (packet_length < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "recv: %s\n", strerror(errno));
            break;
        }

        const struct iphdr *ip;
        const struct udphdr *udp;
        if (!parse_udp_header(packet, (size_t)packet_length, &ip, &udp))
            continue;

        struct timeval now;
        char source[INET_ADDRSTRLEN];
        char destination[INET_ADDRSTRLEN];
        gettimeofday(&now, NULL);
        if (!inet_ntop(AF_INET, &ip->saddr, source, sizeof(source))
            || !inet_ntop(AF_INET, &ip->daddr, destination, sizeof(destination)))
            continue;

        fprintf(output, "%ld\t%ld\t%s\t%u\t%s\t%u\t%u\n",
            (long)now.tv_sec, (long)now.tv_usec,
            source, ntohs(udp->source),
            destination, ntohs(udp->dest),
            ntohs(udp->len));
        records++;
        fflush(output);
    }

    fprintf(stderr, "Captured %lu UDP metadata records on %s\n", records, argv[1]);
    fclose(output);
    close(socket_fd);
    return 0;
}
