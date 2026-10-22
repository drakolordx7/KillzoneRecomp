# usage in python: from crlf_edit import edit; edit(path, [(old, new), ...])  -- exact replacements on text with '\n', preserving the file's line endings (CRLF or LF), each old must occur once
import sys
def edit(path, pairs):
    b = open(path, 'rb').read()
    crlf = b.count(b'\r\n') > b.count(b'\n') // 2
    t = b.decode('utf-8').replace('\r\n', '\n') if crlf else b.decode('utf-8')
    for old, new in pairs:
        assert t.count(old) == 1, (path, old, t.count(old))
        t = t.replace(old, new)
    open(path, 'wb').write((t.replace('\n', '\r\n') if crlf else t).encode('utf-8'))


def edit_inline(path, pairs):
    """Byte-exact single-line substring replacements (old/new must not contain newlines) and insert-after-line:
    a pair (old, new) replaces a substring; a pair ('+after:<substring of an anchor line>', 'line1\nline2') inserts the lines after that anchor line
    using the anchor line's own line ending. Line endings elsewhere stay untouched (for files with mixed endings)."""
    b = open(path, 'rb').read()
    for old, new in pairs:
        if old.startswith('+after:'):
            key = old[7:].encode('utf-8')
            assert b.count(key) == 1, (path, old, b.count(key))
            i = b.index(key)
            j = b.index(b'\n', i)
            eol = b'\r\n' if b[j - 1:j] == b'\r' else b'\n'
            ins = eol.join(l.encode('utf-8') for l in new.split('\n')) + eol
            b = b[:j + 1] + ins + b[j + 1:]
        else:
            k, v = old.encode('utf-8'), new.encode('utf-8')
            assert b.count(k) == 1, (path, old, b.count(k))
            b = b.replace(k, v)
    open(path, 'wb').write(b)
