package main

import "fmt"

func Greet(name string) string {
    n := 0x1f
    if n > 0 {
        return fmt.Sprintf("hi %s", name)
    }
    return name
}

func main() {
    fmt.Println(Greet("world"))
}
