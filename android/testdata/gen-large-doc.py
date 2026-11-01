# Writes a long test document (about 3000 lines of prose, tables and code) for timing the find bar.
#   python3 gen-large-doc.py > find-large.md
import random

random.seed(7)
words = ("the quick brown fox jumps over a lazy dog while another fox watches from the hill and the river runs "
         "past old stone bridges under pale morning light so that travellers rest, read, write and wonder about "
         "markdown tables, code blocks and long paragraphs of ordinary prose").split()


def sentence():
    return " ".join(random.choice(words) for _ in range(random.randint(8, 18))).capitalize() + "."


out = ["# Large find test", ""]
n = 0
section = 0
while len(out) < 3000:
    section += 1
    out += [f"## Section {section}", ""]
    for _ in range(4):
        out += [" ".join(sentence() for _ in range(random.randint(2, 5))), ""]
    out += ["| name | value | note |", "|---|---|---|"]
    for r in range(8):
        out.append(f"| item {section}.{r} | {random.randint(1, 999)} | {sentence()} |")
    out += ["", "```python"]
    for r in range(6):
        out.append(f"def fox_{section}_{r}(x): return x * {r} + len('{random.choice(words)}')")
    out += ["```", ""]
print("\n".join(out))
