import builtins, os, tempfile

def print(*args, sep=" ", end="\n", **kw):
    if not hasattr(print, "fd"):
        path = os.path.join(tempfile.gettempdir(), "gdb-term")
        if not os.path.exists(path):
            os.mkfifo(path, 0o600)
        print.fd = os.open(path, os.O_RDWR)
        builtins.print("reading:  cat " + path)
    os.write(print.fd, (sep.join(str(a) for a in args) + end).encode())

builtins.print = print

print('1111111111111')