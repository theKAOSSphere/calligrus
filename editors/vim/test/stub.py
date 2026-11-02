# Stand-in for `calligrus --editor`: appends every stdin line to the file named by $CALLIGRUS_STUB_LOG.
# Optional: if $CALLIGRUS_STUB_OUT names a file, every line appended to it later is written to
# stdout as ONE write (flushed), after turning the two characters `\n` into a newline. That is
# how tests make the "app" send `goto` lines, split across writes, malformed, and so on.
import os, sys, threading, time
log = open(os.environ["CALLIGRUS_STUB_LOG"], "a", encoding="utf-8", newline="\n")
log.write("ARGS " + " ".join(sys.argv[1:]) + "\n"); log.flush()

def feed(path):
    while not os.path.exists(path):
        time.sleep(0.01)
    with open(path, "rb") as f:
        while True:
            line = f.readline()
            if not line.endswith(b"\n"):  # nothing yet, or half a line: wait for the rest
                f.seek(f.tell() - len(line))
                time.sleep(0.01)
                continue
            chunk = line[:-1].replace(b"\\n", b"\n")
            sys.stdout.buffer.write(chunk); sys.stdout.buffer.flush()

if os.environ.get("CALLIGRUS_STUB_OUT"):
    threading.Thread(target=feed, args=(os.environ["CALLIGRUS_STUB_OUT"],), daemon=True).start()
for raw in sys.stdin.buffer:  # bytes: the protocol is UTF-8 whatever the console code page
    log.write(raw.decode("utf-8")); log.flush()
log.write("EOF\n"); log.flush()
