"""Python syntax fixture."""

def greet(name: str) -> str:
    # Comment and formatted string.
    return f"Hello, {name or 'world'}!"

print(greet("Ada"))
