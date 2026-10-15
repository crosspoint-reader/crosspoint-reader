from pathlib import Path
import re
import sys
source = Path(sys.argv[1]).read_text()
limit = re.search(r"constexpr size_t MAX_TOKEN_FILE_SIZE = [^;]+;", source)
assert limit
start = source.index("namespace pluginhttp {")
end = source.index("\nvoid loadConfigFile(", start)
Path(sys.argv[2]).write_text(limit.group() + "\n" + source[start:end] + "\n} // namespace pluginhttp\n")
