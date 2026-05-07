#!/bin/bash
# Usage: gen_icon_header.sh <input.png> <var_name> <output.h>
PNG="$1"
VAR="$2"
OUT="$3"

xxd -i "$PNG" \
    | sed "s/unsigned char [^[]*\[\]/unsigned char ${VAR}[]/" \
    | sed "s/unsigned int [^ ]*/unsigned int ${VAR}Len/" \
    > "$OUT"
