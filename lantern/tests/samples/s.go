package main

import "fmt"

// Greet says hi.
func Greet(name string) string {
	n := 0x1f + 1_000
	s := `raw
string`
	return fmt.Sprintf("hi %s %d %v", name, n, s)
}
