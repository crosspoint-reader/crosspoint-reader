import argparse
import struct
import sys
import os

# TODO: sync with ResourcesFS.h

MAX_FILES = 32
MAX_FILE_NAME_LENGTH = 32
ALIGNMENT = 4
MAGIC = 0x46535631
MAX_ALLOC_SIZE = 3 * 1024 * 1024

filetype_map = {
  'INVALID': 0,
  'FONT_REGULAR': 1,
}

def main():
  parser = argparse.ArgumentParser(description='Generate resources.bin')
  parser.add_argument('-o', default='resources.bin', help='specify output binary file (default: resources.bin)')
  parser.add_argument('inputs', nargs='*', help='file1:type1 file2:type2 ... (note: if file name contains extension, it will be stripped; example: my_font.bin:FONT_REGULAR will be stored as "my_font")')
  args = parser.parse_args()

  write_data = bytearray()
  write_data += struct.pack('<I', MAGIC)
  for _ in range(MAX_FILES):
    write_data += struct.pack('<II32s', 0, 0, b'\x00' * 32)
  current_size = len(write_data)

  for inp in args.inputs:
    try:
      file_path, type_str = inp.split(':')
    except ValueError:
      print(f"Invalid input format: {inp}. Expected file:type")
      sys.exit(1)

    basename = os.path.basename(file_path)
    name = os.path.splitext(basename)[0]
    name_len = len(name.encode('ascii'))
    if name_len > MAX_FILE_NAME_LENGTH - 1:
      print(f"File name too long: {name} (max {MAX_FILE_NAME_LENGTH - 1} chars)")
      sys.exit(1)
    name_bytes = name.encode('ascii') + b'\x00' * (MAX_FILE_NAME_LENGTH - name_len)

    if type_str.isdigit():
      type_int = int(type_str)
    else:
      upper_type = type_str.upper()
      if upper_type in filetype_map:
        type_int = filetype_map[upper_type]
      else:
        print(f"Unknown file type: {type_str}")
        sys.exit(1)

    try:
      with open(file_path, 'rb') as f:
        data = f.read()
    except IOError as e:
      print(f"Error reading file {file_path}: {e}")
      sys.exit(1)

    size = len(data)
    if size == 0:
      print(f"Invalid file entry: empty file {file_path}")
      sys.exit(1)
    if size % ALIGNMENT != 0:
      print(f"File size must be multiple of alignment ({ALIGNMENT}): {file_path} size={size}")
      sys.exit(1)
    if current_size + size > MAX_ALLOC_SIZE:
      print(f"Not enough space in ResourcesFS image for {file_path}")
      sys.exit(1)

    # Find empty slot and check for duplicates
    found = -1
    for i in range(MAX_FILES):
      offset = 4 + i * 40
      t, s, n = struct.unpack_from('<II32s', write_data, offset)
      name_existing = n.split(b'\x00', 1)[0].decode('ascii', errors='ignore')
      if t == 0:
        if found == -1:
          found = i
      elif name_existing == name:
        print(f"File with the same name already exists: {name}")
        sys.exit(1)

    if found == -1:
      print("No empty slot available")
      sys.exit(1)

    # Set entry
    offset = 4 + found * 40
    write_data[offset:offset + 8] = struct.pack('<II', type_int, size)
    write_data[offset + 8:offset + 40] = name_bytes

    # Append data
    write_data += data
    current_size += size

    print(f"Added file: {name}, type={type_int}, size={size}")

  try:
    with open(args.o, 'wb') as f:
      f.write(write_data)
  except IOError as e:
    print(f"Error writing output file {args.o}: {e}")
    sys.exit(1)

if __name__ == '__main__':
  main()
