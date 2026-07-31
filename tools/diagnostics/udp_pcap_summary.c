#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct PcapHeader {
    uint32_t magic;
    uint16_t major;
    uint16_t minor;
    int32_t zone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t linktype;
};

struct RecordHeader {
    uint32_t seconds;
    uint32_t microseconds;
    uint32_t capturedLength;
    uint32_t originalLength;
};

struct Endpoint {
    uint32_t address;
    uint16_t port;
    unsigned long packets;
    unsigned long bytes;
};

enum { MaxEndpoints = 64 };

static void addEndpoint(struct Endpoint* endpoints, uint32_t address, uint16_t port, unsigned long bytes)
{
    size_t i;
    size_t freeIndex = MaxEndpoints;

    for (i = 0; i < MaxEndpoints; ++i) {
        if (endpoints[i].packets && endpoints[i].address == address && endpoints[i].port == port) {
            endpoints[i].packets++;
            endpoints[i].bytes += bytes;
            return;
        }
        if (!endpoints[i].packets && freeIndex == MaxEndpoints)
            freeIndex = i;
    }
    if (freeIndex == MaxEndpoints)
        return;
    endpoints[freeIndex].address = address;
    endpoints[freeIndex].port = port;
    endpoints[freeIndex].packets = 1;
    endpoints[freeIndex].bytes = bytes;
}

static int compareEndpoints(const void* left, const void* right)
{
    const struct Endpoint* a = left;
    const struct Endpoint* b = right;
    if (a->bytes < b->bytes)
        return 1;
    if (a->bytes > b->bytes)
        return -1;
    return 0;
}

int main(int argc, char** argv)
{
    FILE* input;
    struct PcapHeader pcap;
    struct Endpoint inbound[MaxEndpoints] = { 0 };
    struct Endpoint outbound[MaxEndpoints] = { 0 };
    unsigned long records = 0;
    unsigned long udpPackets = 0;
    unsigned long malformed = 0;
    size_t i;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <capture.pcap>\n", argv[0]);
        return 2;
    }
    input = fopen(argv[1], "rb");
    if (!input) {
        perror(argv[1]);
        return 1;
    }
    if (fread(&pcap, sizeof(pcap), 1, input) != 1 || pcap.magic != 0xa1b2c3d4 || pcap.linktype != 1) {
        fprintf(stderr, "Unsupported pcap header\n");
        fclose(input);
        return 1;
    }

    for (;;) {
        struct RecordHeader record;
        uint8_t* frame;
        size_t offset = 14;
        uint16_t etherType;
        struct iphdr* ip;
        struct udphdr* udp;

        if (fread(&record, sizeof(record), 1, input) != 1)
            break;
        records++;
        if (!record.capturedLength || record.capturedLength > 65535) {
            malformed++;
            break;
        }
        frame = malloc(record.capturedLength);
        if (!frame || fread(frame, record.capturedLength, 1, input) != 1) {
            free(frame);
            malformed++;
            break;
        }
        if (record.capturedLength < offset) {
            malformed++;
            free(frame);
            continue;
        }
        etherType = ((uint16_t)frame[12] << 8) | frame[13];
        if (etherType == 0x8100 || etherType == 0x88a8) {
            if (record.capturedLength < offset + 4) {
                malformed++;
                free(frame);
                continue;
            }
            etherType = ((uint16_t)frame[offset + 2] << 8) | frame[offset + 3];
            offset += 4;
        }
        if (etherType != 0x0800 || record.capturedLength < offset + sizeof(struct iphdr)) {
            free(frame);
            continue;
        }
        ip = (struct iphdr*)(frame + offset);
        offset += (size_t)ip->ihl * 4;
        if (ip->version != 4 || ip->protocol != IPPROTO_UDP || record.capturedLength < offset + sizeof(struct udphdr)) {
            free(frame);
            continue;
        }
        udp = (struct udphdr*)(frame + offset);
        udpPackets++;
        addEndpoint(inbound, ip->saddr, ntohs(udp->source), record.originalLength);
        addEndpoint(outbound, ip->daddr, ntohs(udp->dest), record.originalLength);
        free(frame);
    }
    fclose(input);

    qsort(inbound, MaxEndpoints, sizeof(*inbound), compareEndpoints);
    qsort(outbound, MaxEndpoints, sizeof(*outbound), compareEndpoints);
    printf("records=%lu udp=%lu malformed=%lu\n", records, udpPackets, malformed);
    for (i = 0; i < MaxEndpoints && inbound[i].packets; ++i) {
        struct in_addr address = { .s_addr = inbound[i].address };
        printf("in  %s:%u packets=%lu bytes=%lu\n", inet_ntoa(address), inbound[i].port, inbound[i].packets, inbound[i].bytes);
    }
    for (i = 0; i < MaxEndpoints && outbound[i].packets; ++i) {
        struct in_addr address = { .s_addr = outbound[i].address };
        printf("out %s:%u packets=%lu bytes=%lu\n", inet_ntoa(address), outbound[i].port, outbound[i].packets, outbound[i].bytes);
    }
    return 0;
}
