# Two columns

<!-- column_layout: [1, 1] -->

<!-- column: 0 -->

## Left

- one
- two
- three

<!-- column: 1 -->

## Right

Some text on the right, long enough to wrap onto a second line in a narrow column.

<!-- reset_layout -->

Full width again, below both columns.

---

## Weighted, with a pause

<!-- column_layout: [2, 1] -->

<!-- column: 0 -->

```rust
fn main() {
    println!("wide column");
}
```

<!-- column: 1 -->

<!-- pause -->

$$E = mc^2$$

<!-- pause -->

> A quote, last.
