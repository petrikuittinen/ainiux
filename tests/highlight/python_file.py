"""Python syntax-highlighting fixture.

This module deliberately combines common declarations, literals, annotations,
control flow, comprehensions, decorators, and a multiline string.
"""

from dataclasses import dataclass
from typing import Any, Callable


def traced(function: Callable[..., str]) -> Callable[..., str]:
    """Return a small decorator that reports each method call."""

    def wrapper(*args: Any, **kwargs: Any) -> str:
        print(f"calling {function.__name__}")
        return function(*args, **kwargs)

    return wrapper


@dataclass
class Greeter:
    """A class with attributes, instance methods, and a class method."""

    name: str
    enabled: bool = True

    @traced
    def greet(self, punctuation: str = "!") -> str:
        message = """Hello from a string
which spans multiple lines
and remains one Python value."""
        return f"{message}\nWelcome, {self.name or 'world'}{punctuation}"

    @classmethod
    def from_mapping(cls, values: dict[str, object]) -> "Greeter":
        return cls(name=str(values.get("name", "world")))


names: list[str] = ["Ada", "Grace", "Linus"]
scores: dict[str, int] = {"Ada": 100, "Grace": 98, "Linus": 95}
active_names = [name.upper() for name in names if scores.get(name, 0) >= 98]

greeter = Greeter.from_mapping({"name": active_names[0]})
if greeter.enabled:
    print(greeter.greet())
