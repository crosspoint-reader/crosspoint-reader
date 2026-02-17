Import("env")
import os
import sys

fontdata_path = os.path.join(env.subst("$PROJECT_DIR"), "fontdata.bin")
partition_offset = "0x810000"

def upload_fontdata(source, target, env):
    if not os.path.exists(fontdata_path):
        print(f"ERROR: {fontdata_path} not found.")
        print("Run lib/EpdFont/scripts/convert-builtin-fonts.sh to generate it.")
        env.Exit(1)

    size = os.path.getsize(fontdata_path)
    print(f"Uploading fontdata.bin ({size} bytes, {size/1024:.1f} KB) to {partition_offset}")

    cmd = (
        '"' + env.subst("$PYTHONEXE") + '"'
        + ' "' + env.subst("$UPLOADER") + '"'
        + " --chip " + env.subst("$BOARD_MCU")
    )
    upload_port = env.subst("$UPLOAD_PORT")
    if upload_port:
        cmd += " --port " + upload_port
    upload_speed = env.subst("$UPLOAD_SPEED")
    if upload_speed:
        cmd += " --baud " + upload_speed
    cmd += " write_flash " + partition_offset + " " + fontdata_path

    env.Execute(cmd)

env.AddCustomTarget(
    name="uploadfontdata",
    dependencies=None,
    actions=[upload_fontdata],
    title="Upload Font Data",
    description="Flash fontdata.bin to the fontdata partition",
)
