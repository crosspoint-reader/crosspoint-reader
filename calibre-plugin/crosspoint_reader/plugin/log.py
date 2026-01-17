import time


_LOG = []
_MAX_LINES = 200


def add_log(message):
    timestamp = time.strftime('%H:%M:%S')
    line = f'[{timestamp}] {message}'
    _LOG.append(line)
    if len(_LOG) > _MAX_LINES:
        _LOG[:len(_LOG) - _MAX_LINES] = []


def get_log_text():
    return '\n'.join(_LOG)
