import { readFile } from "fs/promises";

const MAX_RETRIES = 3;

/** Fetch a file and count its lines. */
export async function countLines(path) {
  const text = await readFile(path, "utf8");
  return text.split(/\r?\n/).length;
}

class Counter extends EventTarget {
  #count = 0;
  increment(by = 1) {
    this.#count += by;
    console.log(`count is ${this.#count}`);
    return this;
  }
}

const c = new Counter();
for (let i = 0; i < MAX_RETRIES; i++) c.increment(i * 2.5);
const el = <div className="box">{c.count ?? null}</div>;
