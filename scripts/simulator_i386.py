Import("env")

# PlatformIO's native environment compiles with build_flags, but the final link
# step can still default to the host architecture. Force -m32 into the actual
# compiler and linker flag buckets used by SCons so the executable matches the
# object files.
env.Append(
    CCFLAGS=["-m32"],
    LINKFLAGS=["-m32"],
)
