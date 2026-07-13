"use strict";

const defaults = Object.freeze({
  greeting: "Hello",
  punctuation: "!",
  enabled: true,
  retries: 3,
  metadata: null,
});

class Greeter {
  static category = "example";

  constructor(name = "world", options = {}) {
    this.name = name;
    this.options = { ...defaults, ...options };
    this.messages = [];
  }

  get label() {
    return `${this.options.greeting}, ${this.name}${this.options.punctuation}`;
  }

  add(...messages) {
    this.messages.push(...messages.filter(Boolean));
    return this;
  }

  *[Symbol.iterator]() {
    yield* this.messages;
  }

  async loadProfile(fetchProfile) {
    const profile = await fetchProfile?.(this.name);
    return profile ?? { name: this.name, roles: [] };
  }
}

const people = ["Ada", "Grace", "Linus"];
const scores = { Ada: 100, Grace: 98, Linus: 95 };
const [first, ...others] = people;
const { greeting, punctuation: mark } = defaults;

for (let index = 0; index < people.length; index += 1) {
  console.log(index, people[index]);
}

for (const person of people) {
  console.log(`${person}: ${scores[person]}`);
}

for (const property in scores) {
  if (Object.hasOwn(scores, property)) {
    console.log(property, scores[property]);
  }
}

people.forEach((person, index) => console.log({ person, index }));

let countdown = 2;
do {
  countdown -= 1;
} while (countdown > 0);

while (others.length > 0) {
  others.pop();
}

const greeter = new Greeter(first, { greeting, punctuation: mark })
  .add("Welcome", "Enjoy your stay");

for (const message of greeter) {
  console.log(`${greeter.label}: ${message}`);
}

greeter
  .loadProfile(async (name) => ({ name, roles: ["admin"] }))
  .then(({ name, roles = [] }) => console.log(name, roles.join(", ")))
  .catch((error) => console.error(error?.message ?? "Unknown error"));
