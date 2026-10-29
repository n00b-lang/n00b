package main

import "os"

func fib(n int64) int64 {
	if n < 2 {
		return n
	}
	return fib(n-1) + fib(n-2)
}

func main() {
	if fib(34+int64(len(os.Args))) != 9227465 {
		os.Exit(1)
	}
}
