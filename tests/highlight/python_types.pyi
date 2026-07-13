from typing import Protocol

class Greeter(Protocol):
    enabled: bool
    def greet(self, name: str = ...) -> str: ...
