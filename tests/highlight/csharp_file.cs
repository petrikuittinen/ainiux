using System;

record Person(string Name, int Age);
var person = new Person("Ada", 36);
Console.WriteLine($"{person.Name}: {person.Age}");
