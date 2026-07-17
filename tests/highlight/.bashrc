# Interactive Bash startup fixture.
export EDITOR="ainiux --editor"
alias ll='ls -la'
greet() { printf 'Hello, %s\n' "${1:-world}"; }
