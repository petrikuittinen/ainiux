# Interactive Bash startup fixture.
export EDITOR="pkchat --editor"
alias ll='ls -la'
greet() { printf 'Hello, %s\n' "${1:-world}"; }
