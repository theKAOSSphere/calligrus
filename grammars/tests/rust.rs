use std::collections::HashMap;

/// A point in 2-D space.
#[derive(Debug, Clone)]
pub struct Point<T> {
    x: T,
    y: T,
}

const MAX_SIZE: usize = 0x10;

impl Point<f64> {
    fn norm(&self) -> f64 {
        (self.x * self.x + self.y * self.y).sqrt()
    }
}

fn main() {
    // greet everyone
    let mut seen: HashMap<&str, i32> = HashMap::new();
    let s = "héllo\twörld ✓";
    let c = 'x';
    for (i, word) in s.split(' ').enumerate() {
        if i > MAX_SIZE { break; }
        *seen.entry(word).or_insert(0) += 1;
    }
    println!("{:?} {} {}", seen, c, Some(3.5).is_some());
    let p = Point { x: 1.0, y: 2.0 };
    match p.norm() as u8 {
        0 => {}
        n @ 1..=9 => println!("{n}"),
        _ => unreachable!(),
    }
}
