# Code

```rust
// Rust
fn main() {
    let v: Vec<u32> = (1..=5).collect();
    println!("sum = {}", v.iter().sum::<u32>());
}
```

```python
# Python
def fib(n: int) -> int:
    return n if n < 2 else fib(n - 1) + fib(n - 2)

print([fib(i) for i in range(10)], "done")
```

```cpp
#include <iostream>
template <class T> T twice(T x) { return x + x; }
int main() { std::cout << twice(21) << "\n"; }
```

```json
{ "name": "demo", "n": 3, "ok": true, "list": [1, 2, null] }
```

```bash
for f in *.md; do echo "$f"; done
```
