# Network game-loading speed - what others do (research 27 Oct 2026)

Trigger: MK1 on DIGI5 (Wi-Fi) ~10 MB/s, 22 ms RTT, 153 ms avg per 256 KB request, requests queued for connections;
mac1 imgserve answers each in 1-36 ms. Per connection ~1.9 MB/s = what a ~64 KB TCP window gives at 22 ms.
The PS5 Wi-Fi 6 radio does 450-490 Mbps measured, so the radio is not the limit.

Ranked techniques (sources):
1. Bigger socket buffers. ShadowMountPlus: >=512 KiB socket buffers, 1 MiB requests, 2-4 MiB server buffers
   (github.com/drakmor/ShadowMountPlus). PS5-Upload-Suite: 16 MB buffers + TCP_NODELAY + 16 conns, 30-36 MB/s/file,
   ~280 MB/s on 2.5GbE (github.com/manos555555/PS5-Upload-Suite; Wi-Fi "significantly slower", developer claims).
   OURS: link asks SO_RCVBUF 1 MiB (rcvbuf_kb 1024) but never reads back what the console GRANTS -> log it first.
2. More bytes in flight: 1 MiB requests (SMP+) / up to 32 concurrent requests (github.com/sweenyxsky-oss/PS5-Network-Mounting);
   playden-ps5 #29 fixed slow downloads by raising slots/ahead. OURS: DIGI5 mount.conf conns 16, readahead_kb 4096 (27 Oct).
3. Trace-driven prefetch + merging adjacent reads (ampr_emu ASSET_PACK_WORKFLOW_EN.md; Blackwire SMB keeps startup files local,
   64 MiB RAM cache, batched reads: 1038 -> 275 source reads in a host test). OURS: imgserve hot set (8a35de9) is server-side only.
4. Fewer bytes: ampr_emu LZ4 packs (~half size, 30-40% faster locally, developer claim). zlib .ffpfsc is SLOWEST to load
   (SMP+ #105, Nioh 3 local: folder 21.8 s, FFPKG 43.6 s, exFAT 90.9 s, ffpfsc 119 s).
5. Wired beats everything; no PS5-specific QoS/port advice found.
Known: ampr_emu stutters on texture streaming even locally (#4 FF7R, #11 Mafia). No published PS5 network-load benchmarks
(Blackwire: "pending"). Reddit search was blocked to the agent; one snippet: 8.6 MiB/s FTP on 6E Wi-Fi.

Plan: (a) log granted SO_RCVBUF per link connection; (b) if small, find the console's cap and raise it; server SO_SNDBUF on
imgserve >= 4 MiB; (c) blazingmountd auto-tunes conns/readahead when the console is on Wi-Fi; (d) try 1 MiB block_kb;
(e) LZ4 packs instead of zlib .ffpfsc for streamed games.
