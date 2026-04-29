#!/bin/bash
# Build the 16-bit DOS door (anetirc.exe) with OpenWatcom.

set -euo pipefail

OUTDIR="build"; OBJDIR="$OUTDIR/obj"; mkdir -p "$OUTDIR" "$OBJDIR"

OW_REL="${WATCOM:-/mnt/hdd2/open/rel}"; export WATCOM="$OW_REL"

if [ -d "$WATCOM/binl64" ]; then export PATH="$WATCOM/binl64:$PATH"; fi
if [ -d "$WATCOM/binl" ]; then export PATH="$WATCOM/binl:$PATH"; fi

inc_list=""
for d in "$WATCOM/h" "$WATCOM/h/dos" "$WATCOM/h/os2" "$WATCOM/h/nt"; do [ -d "$d" ] || continue; if [ -z "$inc_list" ]; then inc_list="$d"; else inc_list="$inc_list;$d"; fi; done
export INCLUDE="$inc_list"

SRCS=(main.c fossil.c dropfile.c bridge.c); OBJS=()

for src in "${SRCS[@]}"; do
    obj="$OBJDIR/$(basename "${src%.c}").obj"
    wcc -bt=dos -mc -os -5 -fo="$obj" "$src"
    OBJS+=("$obj")
done

LNKFILE="$OUTDIR/anetirc_dosdoor.lnk"

{
    echo "system dos"
    echo "name $OUTDIR/anetirc.exe"
    echo "option map=$OUTDIR/anetirc_dosdoor.map"
    echo "option stack=32k"
    for obj in "${OBJS[@]}"; do echo "file $obj"; done
} > "$LNKFILE"

wlink @"$LNKFILE"

echo "Built: $OUTDIR/anetirc.exe"
