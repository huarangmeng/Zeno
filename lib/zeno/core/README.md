# Zeno core builtin package

Stage0 treats this directory as the reserved `core` declaration package.
The `.zn` files record the first builtin signatures, layout facts, runtime
requirements, and intrinsic boundaries used by compiler metadata and cache
fingerprints. Full library implementations can replace declaration-only items
incrementally.
