# Usage

- `cp -r ./module <path/to/kernel/src>/arch/arm64/parasyte`
- `cp ./headers/asm/parasyte.h <path/to/kernel/src>/arch/arm64/include/asm`
- `cp ./headers/uapi/asm/parasyte.h <path/to/kernel/src>/arch/arm64/include/uapi/asm`
- Apply patches from `./patches`
- Set `CONFIG_PARASYTE_HIVE=y` to build host kernel (default), `CONFIG_PARASYTE_SPORE=y` for guest.
