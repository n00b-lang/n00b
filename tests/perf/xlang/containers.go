package main

import "os"

func run(rounds, size int64) int64 {
	var total int64
	for round := int64(0); round < rounds; round++ {
		var items []int64
		table := map[int64]int64{}
		for i := int64(0); i < size; i++ {
			items = append(items, i)
			table[i] = i * 2
		}
		for _, item := range items {
			if found, ok := table[item]; ok {
				total += found
			}
		}
	}
	return total
}

func main() {
	if run(50*int64(len(os.Args)), 200000) != 1999990000000 {
		os.Exit(1)
	}
}
