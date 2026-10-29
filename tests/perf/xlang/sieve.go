package main

import "os"

func countPrimes(limit int64) int64 {
	composite := make([]bool, limit+1)
	var count int64
	for n := int64(2); n <= limit; n++ {
		if composite[n] {
			continue
		}
		count++
		for multiple := n * n; multiple <= limit; multiple += n {
			composite[multiple] = true
		}
	}
	return count
}

func main() {
	var total int64
	for round := int64(0); round < 6*int64(len(os.Args)); round++ {
		total += countPrimes(2000000)
	}
	if total != 893598 {
		os.Exit(1)
	}
}
