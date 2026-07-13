#!/usr/bin/env bash

declare -a values=(one two three)
for value in "${values[@]}"; do
  echo "value=$value"
done
