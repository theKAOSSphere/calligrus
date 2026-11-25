#!/usr/bin/env bash
# Checks that a Linux build runs on an old distribution (the release is built on Ubuntu 22.04, glibc 2.35):
#   scripts/check-portable.sh <executable> [max glibc version, default 2.35]
# Fails (exit 1) when
#   - readelf -d lists a NEEDED library other than libc, libm and what glibc 2.35 still keeps apart (libdl, libpthread,
#     librt, the dynamic loader): no libstdc++, libgcc_s, libOpenGL, libGL, libX11...;
#   - objdump -T lists a GLIBC_ symbol version above the maximum, or any GLIBCXX_ / CXXABI_ version (the C++ runtime has to
#     be linked in statically).
# Prints both lists either way. (Plain sh tools only: awk, sort.)
set -u
exe="${1:?usage: check-portable.sh <executable> [max glibc version]}"
max="${2:-2.35}"
fail=0

echo "== NEEDED libraries of $exe"
needed=$(readelf -d "$exe" | awk '/\(NEEDED\)/ { gsub(/[\[\]]/, "", $NF); print $NF }')
printf '%s\n' "$needed"
for lib in $needed; do
    case "$lib" in
        libc.so.6 | libm.so.6 | libdl.so.2 | libpthread.so.0 | librt.so.1 | ld-linux-*.so.* | ld-linux.so.*) ;;
        *) echo "NOT ALLOWED: $lib"; fail=1 ;;
    esac
done

echo "== symbol versions required by $exe (objdump -T)"
versions=$(objdump -T "$exe" | awk '{ for (i = 1; i <= NF; i++) { v = $i; gsub(/[()]/, "", v); if (v ~ /^(GLIBC|GLIBCXX|CXXABI|GCC)_[0-9]/) { print v; break } } }' | sort -u)
printf '%s\n' "$versions"
for v in $versions; do
    case "$v" in
        GLIBC_*)
            ver="${v#GLIBC_}"
            top=$(printf '%s\n%s\n' "$ver" "$max" | sort -V | tail -n 1)
            if [ "$top" != "$max" ]; then echo "TOO NEW: $v (maximum GLIBC_$max)"; fail=1; fi ;;
        *) echo "NOT ALLOWED: $v (the C++ / gcc runtime must be linked statically)"; fail=1 ;;
    esac
done

if [ "$fail" = 0 ]; then echo "portable: ok"; else echo "portable: FAILED"; fi
exit $fail
