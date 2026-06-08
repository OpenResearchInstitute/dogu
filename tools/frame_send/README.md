# tools/frame_send

The A53/dogu network hop for the receive path — the equivalent of what
Dialogus does on the LibreSDR. Reads decoded 134-byte Opulent Voice frames
from stdin (the output of `opv-decode -r`) and sends each whole frame as one
UDP datagram to Interlocutor.

Interlocutor binds its OV-frame socket on **port 57372** and accepts exactly
one 134-byte frame per datagram (it rejects any datagram whose length != 134);
the COBS reassembly Interlocutor does happens *inside* the 122-byte payloads
and is its own concern. `frame_send` therefore never wraps, splits, or adds a
header — `opv-decode`'s 134-byte output already is the OV frame Interlocutor
parses (6-byte station ID + 3-byte token + 3-byte reserved + 122-byte payload).

```
usage:  opv-decode -3 ... -r | frame_send <interlocutor_ip> [port]
        frame_send 10.73.1.42              # default port 57372
```

Build/deploy via the top-level dogu Makefile (`make cross && make deploy`).
