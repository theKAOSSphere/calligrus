package main

import (
	"fmt"
	"strings"
)

// Shape has an area.
type Shape interface {
	Area() float64
}

type Rect struct {
	W, H float64
}

func (r Rect) Area() float64 { return r.W * r.H }

const maxShapes = 10

func main() {
	shapes := []Shape{Rect{W: 2, H: 3.5}}
	names := make(map[string]int, len(shapes))
	for i, s := range shapes {
		if i >= maxShapes {
			break
		}
		names[strings.ToUpper("rect")] += 1
		fmt.Printf("%d: %.2f\n", i, s.Area())
	}
	defer fmt.Println(nil, true, 'x')
}
