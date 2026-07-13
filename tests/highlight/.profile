# POSIX-compatible login fixture detected as Bash.
if [ -d "$HOME/bin" ]; then
  PATH="$HOME/bin:$PATH"
fi
export PATH
