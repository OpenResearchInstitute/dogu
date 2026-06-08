/* frame_send — forward decoded OPV frames to Interlocutor over UDP.
 *
 * Reads a stream of 134-byte Opulent Voice frames on stdin (the output of
 * `opv-decode -r`) and sends each whole frame as one UDP datagram to
 * Interlocutor's OV-frame port (default 57372). This is the dogu/A53
 * equivalent of the network hop Dialogus performs on the LibreSDR: the
 * decode lives in the submodule, the transport lives here.
 *
 * Interlocutor expects exactly one 134-byte OV frame per datagram
 * (it rejects anything whose length != 134); we never wrap or split.
 *
 *   usage:  opv-decode -3 ... | frame_send <interlocutor_ip> [port]
 *           frame_send 10.73.1.42            # default port 57372
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define FRAME_BYTES 134
#define OVP_UDP_PORT 57372

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <interlocutor_ip> [port]\n", argv[0]);
        return 2;
    }
    const char *ip = argv[1];
    int port = (argc >= 3) ? atoi(argv[2]) : OVP_UDP_PORT;

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return 1; }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &dst.sin_addr) != 1) {
        fprintf(stderr, "frame_send: bad IP '%s'\n", ip);
        return 2;
    }

    unsigned char frame[FRAME_BYTES];
    size_t fill = 0;
    unsigned long sent = 0;
    ssize_t n;
    /* Accumulate exactly FRAME_BYTES, then emit one datagram. stdin may
       deliver frames in arbitrary read sizes, so we reassemble. */
    while ((n = read(STDIN_FILENO, frame + fill, FRAME_BYTES - fill)) > 0) {
        fill += (size_t)n;
        if (fill == FRAME_BYTES) {
            if (sendto(s, frame, FRAME_BYTES, 0,
                       (struct sockaddr *)&dst, sizeof(dst)) != FRAME_BYTES)
                perror("sendto");
            else
                ++sent;
            fill = 0;
        }
    }
    if (fill != 0)
        fprintf(stderr, "frame_send: warning: %zu trailing bytes (partial frame) dropped\n", fill);
    fprintf(stderr, "frame_send: forwarded %lu frames to %s:%d\n", sent, ip, port);
    close(s);
    return 0;
}
