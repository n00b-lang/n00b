package com.example;

import java.util.List;

public class Greeter {
    private final String name;
    public String greet(String punct) {
        return "hi " + name + punct;
    }
    public void run() {
        System.out.println(greet("!"));
    }
}
