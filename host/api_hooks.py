#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""api_hooks.py - the five hook lines that wire bm_api.c (the app's local API) into blazingmountd.c.

    python3 -I host/api_hooks.py console/blazingmountd/src/blazingmountd.c

Used by host/test_api.py on a temp copy while the hooks are not in blazingmountd.c itself, and once to put them there.
Each anchor must appear exactly once, or nothing is written.
"""
import sys
p=sys.argv[1]; s=open(p).read()
def rep(a,b,count=1):
    global s
    assert s.count(a)==count, (s.count(a), a)
    s=s.replace(a,b)
rep("// ---------------------------------------------------------------- one pass over the host's games\n",
    "#include \"bm_api.c\"           // the app's local API (:8094): reads + queued actions, applied in sync_once by the one writer\n\n// ---------------------------------------------------------------- one pass over the host's games\n")
rep("    conf_t c; read_conf(&c);\n    check_smp(&c);", "    conf_t c; read_conf(&c);\n    bm_api_pass_begin(&c);                                       // the app's queued actions + its choices (bm_api.c)\n    check_smp(&c);")
rep("                int rc = build_game(&c, &g, cfw, reason, sizeof reason, 1);",
    "                bm_api_busy(g.tid, g.title);\n                int rc = build_game(&c, &g, cfw, reason, sizeof reason, 1);\n                bm_api_busy(NULL, NULL);")
rep("        int rc = build_game(&c, &g, cfw, reason, sizeof reason, 0);",
    "        bm_api_busy(g.tid, g.title);\n        int rc = build_game(&c, &g, cfw, reason, sizeof reason, 0);\n        bm_api_busy(NULL, NULL);")
rep("    for (;;) {\n        conf_t c; read_conf(&c);\n        int rc = sync_once();\n        sleep(rc == 0 ? (unsigned)c.poll_secs : 60);                 // host not up yet (boot): try again in a minute",
    "    bm_api_start();                                                  // the app's API thread (bm_api.c)\n    for (;;) {\n        conf_t c; read_conf(&c);\n        int rc = sync_once();\n        bm_api_wait(rc == 0 ? c.poll_secs : 60);                     // host not up yet (boot): a minute; early when the app asks or the drives change")
open(p,"w").write(s)
print("hooks applied")
