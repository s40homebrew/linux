#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
commit=adc218676eef25575469234709c2d87185ca223a
if [[ ! -f /work/source/.rm443-source-commit ]]; then
    [[ ! -e /work/source ]] || { echo 'Refusing to overlay an unmarked source tree' >&2; exit 1; }
    mkdir /work/source
    git -c safe.directory=/reference -C /reference archive "$commit" | tar -xf - -C /work/source
    printf '%s\n' "$commit" > /work/source/.rm443-source-commit
fi
[[ $(cat /work/source/.rm443-source-commit) == "$commit" ]] || exit 1
python3 /project/prepare.py /work/source
output=/work/output-$(uname -m)
mkdir -p "$output" /out
export ARCH=arm CROSS_COMPILE=arm-linux-gnueabi-
# Docker assigns a new hostname each run; embedding that random ID makes
# compile.h dirty and forces a relink even when all sources are unchanged.
export KBUILD_BUILD_USER=builder KBUILD_BUILD_HOST=rm443-linux
if [[ ! -f "$output/.rm443-uapi-ready" ]]; then
    make -C /work/source O="$output" -j"${JOBS:-4}" headers
    touch "$output/.rm443-uapi-ready"
fi
bash /project/userspace/build.sh
config_input="$output/.rm443-config-fingerprint"
# New board Kconfig selections can introduce symbols without changing the
# fragment. Include all overlay selections and their installer so Kbuild never
# falls into interactive config when a driver moves to a different directory.
config_fingerprint=$({ cat /project/rm443.config /project/prepare.py; find /project/overlay -type f -name 'Kconfig*' -print0 | sort -z | xargs -0 cat; } | sha256sum | cut -d' ' -f1)
if [[ ! -f "$output/.config" ]] || [[ $(cat "$config_input" 2>/dev/null || true) != "$config_fingerprint" ]]; then
    make -C /work/source O="$output" allnoconfig KCONFIG_ALLCONFIG=/project/rm443.config
    printf '%s\n' "$config_fingerprint" > "$config_input"
fi
for symbol in ARCH_NOKIA_RAPS CPU_ARM926T BLK_DEV_LOOP VFAT_FS EXT4_FS MMC MMC_BLOCK MSDOS_PARTITION TMPFS UNIX DRM_RM443 DRM_GEM_DMA_HELPER DRM_FBDEV_EMULATION FB_DEVICE FRAMEBUFFER_CONSOLE RAPS_DMA; do
    grep -qx "CONFIG_$symbol=y" "$output/.config" || {
        echo "Required kernel option CONFIG_$symbol was disabled by dependencies" >&2
        exit 1
    }
done
make -C /work/source O="$output" -j"${JOBS:-4}" Image
# Keep host artifact timestamps stable too, so boot-package builders can use
# ordinary dependency tracking. Metadata changes only when its contents do.
copy_changed() {
    # OrbStack's macOS bind mount rejects GNU cp's hole-punching operation
    # when a changed, larger sparse Image replaces an existing file.
    if ! cmp -s "$1" "$2"; then cp --reflink=never --sparse=never "$1" "$2"; fi
}
copy_changed "$output/arch/arm/boot/Image" /out/Image
copy_changed "$output/vmlinux" /out/vmlinux
copy_changed "$output/.config" /out/kernel.config
copy_changed /work/initramfs/init /out/init
arm-linux-gnueabi-readelf -A "$output/vmlinux" > "$output/elf-attributes.txt"
arm-linux-gnueabi-nm -n "$output/vmlinux" > "$output/symbols.txt"
python3 /project/inspect_image.py /out/Image "$output/symbols.txt" > "$output/image-layout.json"
printf '%s\n' "$commit" > "$output/source-commit.txt"
for metadata in elf-attributes.txt symbols.txt image-layout.json source-commit.txt; do
    copy_changed "$output/$metadata" "/out/$metadata"
done
printf 'Built RM-443 kernel and initramfs.\n'
