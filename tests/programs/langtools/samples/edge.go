package main

func f(a int,
	b int) int {
	x := a +
		b
	x++
	for i := 0; i < 3; i++ {
		if i == 1 {
			continue
		}
		break
	}
	y := []int{1,
		2}
	return x
}
