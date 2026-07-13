const answer = 42;

function greet(name = "world") {
  return `Hello, ${name}!`;
}

console.log(greet(), answer);
