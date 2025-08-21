Unsecure Enclave (dev)

- Lists slots 001..100 under /int/enclave
- Per-slot actions: View, Generate Random (32-byte simple), Delete

Notes
- Keys are stored in files: /int/enclave/slot_XXX.bin with 4-byte header [ver(1), type(1), size(1), 0] then key bytes.
- This is for development only; keys are not protected.
