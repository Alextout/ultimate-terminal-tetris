"""A small terminal emulator on a pseudo terminal.

Enough of an ANSI implementation to run the game for real and read back what
it actually painted: cursor positioning, erase, and the private mode switches.
Colours are parsed and thrown away, since every check here is about which
character ended up in which cell.

Used by the tools next to this file. Not part of the game.
"""

import codecs
import fcntl
import os
import pty
import re
import select
import signal
import struct
import termios
import time

CSI = re.compile(r"\x1b\[[0-9;:?<>=]*[A-Za-z]")


class Screen:
    """The character grid a terminal would be showing."""

    def __init__(self, cols=100, rows=30):
        self.cols = cols
        self.rows = rows
        self.cells = {}
        self.x = 1
        self.y = 1
        self._partial = ""

    def feed(self, data):
        data = self._partial + data
        self._partial = ""
        i = 0
        while i < len(data):
            c = data[i]
            if c == "\x1b":
                m = CSI.match(data, i)
                if not m:
                    # an escape sequence split across two reads
                    if len(data) - i < 24:
                        self._partial = data[i:]
                        return
                    i += 1
                    continue
                body, final = m.group(0)[2:-1], m.group(0)[-1]
                if final == "H":
                    parts = body.split(";")
                    self.y = int(parts[0] or 1)
                    self.x = int(parts[1]) if len(parts) > 1 and parts[1] else 1
                elif final == "J" and body in ("2", ""):
                    self.cells.clear()
                i = m.end()
            elif c in "\r\n":
                i += 1
            else:
                self.cells[(self.x, self.y)] = c
                self.x += 1
                i += 1

    def render(self):
        lines = []
        for y in range(1, self.rows + 1):
            row = "".join(self.cells.get((x, y), " ")
                          for x in range(1, self.cols + 1)).rstrip()
            lines.append(row)
        while lines and not lines[-1]:
            lines.pop()
        return "\n".join(lines)

    def occupied(self):
        """Every cell that is not blank, for comparing two screen states."""
        return {pos: ch for pos, ch in self.cells.items() if ch != " "}

    def playfield_columns(self):
        """The column range framed by the two border glyphs."""
        xs = [x for (x, y), ch in self.cells.items() if ch in "│|"]
        return (min(xs) + 1, max(xs) - 1) if xs else (0, 0)

    def piece_top(self, glyph="█"):
        """Topmost row inside the playfield holding a block."""
        lo, hi = self.playfield_columns()
        rows = [y for (x, y), ch in self.cells.items()
                if ch == glyph and lo <= x <= hi]
        return min(rows) if rows else None


KEYS = {
    "up": b"\x1b[A", "down": b"\x1b[B", "right": b"\x1b[C", "left": b"\x1b[D",
    "enter": b"\r", "space": b" ", "esc": b"\x1b",
}


class Game:
    """The game running on a pseudo terminal."""

    def __init__(self, binary, args=(), cols=100, rows=30, home=None):
        self.screen = Screen(cols, rows)
        # a read can split a multi-byte character, so decoding has to carry
        # the leftover bytes into the next chunk
        self._decode = codecs.getincrementaldecoder("utf-8")("replace").decode
        self.dead = False
        self.bytes_read = 0
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ["TERM"] = "xterm-256color"
            os.environ["COLORTERM"] = "truecolor"
            # keep the tester's own config and high scores out of the way
            os.environ["HOME"] = home or "/nonexistent"
            os.execvp(binary, [binary] + list(args))
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, cols, 0, 0))

    def pump(self, seconds, poll=0.005):
        """Read and interpret output for a while. Returns bytes seen."""
        seen = 0
        deadline = time.time() + seconds
        while time.time() < deadline:
            ready, _, _ = select.select([self.fd], [], [], poll)
            if not ready:
                continue
            try:
                data = os.read(self.fd, 65536)
            except OSError:
                self.dead = True
                return seen
            if not data:
                self.dead = True
                return seen
            seen += len(data)
            self.bytes_read += len(data)
            self.screen.feed(self._decode(data))
        return seen

    def settle(self, quiet=0.2, limit=2.0):
        """Read until the output has been silent for `quiet` seconds."""
        start = last = time.time()
        while time.time() - start < limit:
            ready, _, _ = select.select([self.fd], [], [], 0.01)
            if ready:
                try:
                    data = os.read(self.fd, 65536)
                except OSError:
                    self.dead = True
                    return
                if not data:
                    self.dead = True
                    return
                self.bytes_read += len(data)
                self.screen.feed(self._decode(data))
                last = time.time()
            elif time.time() - last > quiet:
                return

    def key(self, name, wait=0.12):
        try:
            os.write(self.fd, KEYS.get(name, name.encode()))
        except OSError:
            self.dead = True
            return 0
        return self.pump(wait)

    def quit(self):
        """Ask the game to exit and report its status."""
        if not self.dead:
            self.key("q", 0.4)
        status = None
        for _ in range(20):
            try:
                pid, status = os.waitpid(self.pid, os.WNOHANG)
                if pid:
                    return status
            except ChildProcessError:
                return status
            time.sleep(0.05)
        try:
            os.kill(self.pid, signal.SIGKILL)
            _, status = os.waitpid(self.pid, 0)
        except OSError:
            pass
        return status
