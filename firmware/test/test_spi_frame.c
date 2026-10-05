/* test_spi_frame.c - SPI packet framing and CRC (the portable half of the SPI link). */
#include "muster_link_spi.h"
#include <stdio.h>

static int fails = 0;
#define CHECK(c, m) do { if (c) printf("  PASS  %s\n", m); else { printf("  FAIL  %s\n", m); fails++; } } while (0)

int main(void)
{
    uint8_t pkt[MSPI_PKT], out[MSPI_MAX_PAYLOAD], big[MSPI_MAX_PAYLOAD + 1];
    const uint8_t msg[] = { 0x02, 0x81, 4, 0, 2, 9, 9, 9 };
    int n;
    printf("\nSPI framing tests (packet = %u bytes)\n", (unsigned)MSPI_PKT);

    CHECK(mspi_crc16((const uint8_t *)"123456789", 9) == 0x29B1, "CRC-16/CCITT-FALSE check value 0x29B1");

    mspi_pack(pkt, msg, sizeof msg);
    n = mspi_unpack(pkt, out, sizeof out);
    CHECK(n == (int)sizeof msg && memcmp(out, msg, sizeof msg) == 0, "pack/unpack round trip");

    mspi_pack(pkt, 0, 0);
    CHECK(mspi_unpack(pkt, out, sizeof out) == 0, "poll packet (len 0) round trip");

    mspi_pack(pkt, msg, sizeof msg); pkt[5] ^= 0x10;
    CHECK(mspi_unpack(pkt, out, sizeof out) == -1, "single flipped payload bit rejected");

    mspi_pack(pkt, msg, sizeof msg); pkt[0] = 0x00;
    CHECK(mspi_unpack(pkt, out, sizeof out) == -1, "bad sync byte rejected");

    mspi_pack(pkt, msg, sizeof msg); pkt[1] = MSPI_MAX_PAYLOAD + 1;
    CHECK(mspi_unpack(pkt, out, sizeof out) == -1, "oversized length rejected");

    memset(big, 7, sizeof big);
    CHECK(mspi_pack(pkt, big, MSPI_MAX_PAYLOAD + 1) == -1, "pack refuses payload over MTU");
    CHECK(mspi_pack(pkt, big, MSPI_MAX_PAYLOAD) == 0 &&
          mspi_unpack(pkt, out, sizeof out) == (int)MSPI_MAX_PAYLOAD, "full-MTU payload round trip");

    printf("\n%s\n\n", fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return fails ? 1 : 0;
}
