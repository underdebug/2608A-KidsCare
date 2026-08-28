
import builtins, os, shutil, subprocess, tempfile

def print(*args, sep=" ", end="\n", **kw):
    """print() to a separate terminal window, spawned on first call."""
    if not hasattr(print, "fd"):
        path = os.path.join(tempfile.gettempdir(), "gdb-term-%d" % os.getpid())
        if os.path.exists(path):
            os.unlink(path)
        os.mkfifo(path, 0o600)
        print.fd = os.open(path, os.O_RDWR)       # no block, no EOF
        cmd = "clear; cat " + path
        for argv in (["gnome-terminal", "--", "bash", "-c", cmd],
                     ["konsole", "-e", "bash", "-c", cmd],
                     ["xfce4-terminal", "-x", "bash", "-c", cmd],
                     ["xterm", "-e", "bash", "-c", cmd]):
            if shutil.which(argv[0]):
                subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL, start_new_session=True)
                break
        else:
            builtins.print("run in any terminal:  cat " + path)
    os.write(print.fd, (sep.join(str(a) for a in args) + end).encode())

builtins.print = print 

print('aaaaaaaaaaaaaaaaaaa')