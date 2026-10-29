package main

import "os"

var sink string

func run(rounds int64, tail string) uint64 {
	var total uint64
	for round := int64(0); round < rounds; round++ {
		joined := "x" + tail
		sink = joined
		total += uint64(len(joined))
	}
	return total
}

func main() {
	if run(12000000*int64(len(os.Args)), "short tail") != 132000000 {
		os.Exit(1)
	}
}
