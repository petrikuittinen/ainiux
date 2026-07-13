package main

import (
	"context"
	"fmt"
)

type Number interface {
	~int | ~int64 | ~float64
}

type Stack[T any] struct {
	items []T
}

func (stack *Stack[T]) Push(values ...T) {
	stack.items = append(stack.items, values...)
}

func Sum[T Number](values []T) T {
	var total T
	for index, value := range values {
		if index >= 0 {
			total += value
		}
	}
	return total
}

func main() {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	stack := Stack[string]{items: []string{"Ada", "Grace"}}
	stack.Push("Linus")

	message := `Go raw strings
can span multiple lines.`
	select {
	case <-ctx.Done():
		fmt.Println(ctx.Err())
	default:
		fmt.Println(message, Sum([]int{1, 2, 3}), stack.items)
	}
}

