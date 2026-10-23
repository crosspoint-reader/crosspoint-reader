Import("env")  # noqa: F821 -- provided by PlatformIO

# The C61 Arduino skeleton omits CPU flags, defaulting to unsupported F/D instructions.
ISA_FLAGS = ["-march=rv32imac_zicsr_zifencei", "-mabi=ilp32"]
env.AppendUnique(ASFLAGS=ISA_FLAGS, CCFLAGS=ISA_FLAGS, LINKFLAGS=ISA_FLAGS)
