# Summarize villages' LAN traffic (QEMU mcast 230.0.0.1:1234) for N seconds:
# TCP by (src, dst, port) with SYN / RST / data counts, UDP by flow.
import socket, struct, sys, time, collections
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("", 1234))
s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, struct.pack("4s4s", socket.inet_aton("230.0.0.1"), socket.inet_aton("127.0.0.1")))
s.settimeout(1)
tcp = collections.defaultdict(lambda: collections.Counter()); udp = collections.Counter()
end = time.time() + secs
while time.time() < end:
    try: f, _ = s.recvfrom(65536)
    except socket.timeout: continue
    if struct.unpack("!H", f[12:14])[0] != 0x0800: continue
    ip = f[14:]; ihl = (ip[0] & 15) * 4; tot = struct.unpack("!H", ip[2:4])[0]
    src, dst = socket.inet_ntoa(ip[12:16]), socket.inet_ntoa(ip[16:20])
    if ip[9] == 6:
        sp, dp, _, _, off = struct.unpack("!HHIIH", ip[ihl:ihl + 14])
        fl = off & 0x3f; doff = (off >> 12) * 4; pay = tot - ihl - doff
        server = dp if dp < sp else sp
        key = (src, dst, server) if dp == server else (dst, src, server)
        c = tcp[key]
        if fl & 0x02 and not fl & 0x10: c["syn"] += 1
        if fl & 0x04: c["rst"] += 1
        if pay > 0: c["data_pkts"] += 1; c["bytes"] += pay
    elif ip[9] == 17:
        sp, dp = struct.unpack("!HH", ip[ihl:ihl + 4]); udp[(src, dst, dp)] += 1
for (a, b, p), c in sorted(tcp.items()): print(f"tcp {a} -> {b}:{p}  " + " ".join(f"{k}={v}" for k, v in sorted(c.items())))
for (a, b, p), n in sorted(udp.items()): print(f"udp {a} -> {b}:{p}  pkts={n}")
