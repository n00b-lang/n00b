using System;

namespace Example;

public class Greeter {
    private readonly string name;
    public string Greet(string punct) {
        return "hi " + name + punct;
    }
    public void Run() {
        Console.WriteLine(Greet("!"));
    }
}
