// SPDX-License-Identifier: GPL-3.0-or-later
// bm_inst_helper.c - BlazingMount's one-install helper (see bm_ai.h for why it is a separate process). blazingmountd embeds this ELF
// and hands it to elfldr (127.0.0.1:9021) for every install. It connects back to 127.0.0.1:BM_INST_IPC, brings up Sony's installer,
// installs the ONE url it is sent, answers status polls, and exits (Terminate) when blazingmountd says bye or hangs up.
// Host test build: a plain executable (blazingmountd fork+execs it) with the fake installer from bm_ai.h.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "bm_ai.h"

static int rd(int s, void *b, size_t n) { char *p = b; while (n) { ssize_t r = read(s, p, n); if (r <= 0) return -1; p += r; n -= (size_t)r; } return 0; }
static int wr(int s, const void *b, size_t n) { const char *p = b; while (n) { ssize_t w = write(s, p, n); if (w <= 0) return -1; p += w; n -= (size_t)w; } return 0; }

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(BM_INST_IPC); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a)) return 1;
    struct timeval tv = { 120, 0 };                                // blazingmountd polls every second; silence = it is gone
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int up = ai_bring_up() == 0, installed = 0;
    ih_req_t q; ih_resp_t r;
    while (rd(s, &q, sizeof q) == 0 && q.magic == IH_MAGIC && q.op != IH_BYE) {
        memset(&r, 0, sizeof r); r.magic = IH_MAGIC;
        q.uri[sizeof q.uri - 1] = q.name[sizeof q.name - 1] = q.cid[sizeof q.cid - 1] = 0;
        if (!up) { r.rc = -1; snprintf(r.status, sizeof r.status, "unavailable"); snprintf(r.cid, sizeof r.cid, "%.47s", g_ai_why); }
        else if (q.op == IH_INSTALL) {
            if (installed) r.rc = (int32_t)AI_INVALID_SLOT;            // one per process, by design
            else { r.rc = ai_install_uri(q.uri, q.name, r.cid, sizeof r.cid); installed = r.rc == 0; }
        } else if (q.op == IH_STATUS) {
            ai_status_t *st = calloc(1, sizeof *st);
            r.rc = st ? ai_get_status(q.cid, st) : -1;
            if (st && r.rc == 0) {
                snprintf(r.status, sizeof r.status, "%.15s", st->status);
                r.downloaded = st->downloaded_size; r.total = st->total_size; r.remain = st->remain_time; r.error = st->error_info.error_code;
            }
            free(st);
        } else r.rc = -1;
        if (wr(s, &r, sizeof r)) break;
    }
    if (up && ai_term) ai_term();
    close(s);
    return 0;
}
