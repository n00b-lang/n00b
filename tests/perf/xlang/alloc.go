package main

import "os"

type node struct {
	value int64
	next  *node
}

func run(rounds, length int64) int64 {
	var total int64
	for round := int64(0); round < rounds; round++ {
		var head *node
		for i := int64(0); i < length; i++ {
			head = &node{value: i, next: head}
		}
		for cursor := head; cursor != nil; cursor = cursor.next {
			total += cursor.value
		}
	}
	return total
}

func main() {
	if run(60*int64(len(os.Args)), 100000) != 299997000000 {
		os.Exit(1)
	}
}
