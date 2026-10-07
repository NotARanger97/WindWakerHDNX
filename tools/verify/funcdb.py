"""Index of the recompiled functions (build/gen) plus names, GameCube signatures and the
argument registers each function reads (live-in analysis over the generated C).

Used by mkunit.py (harness units) and mktap.py (recording taps).
"""
import os
import pickle
import re

FUNC_RE = re.compile(r"^void (f_[0-9A-F]{8}(?:_orig|_abi|_sync)?)\(Cpu\* __restrict c\) \{$")
CALL_RE = re.compile(r"\bf_([0-9A-F]{8})(_orig|_abi|_sync)?\(c\)")
INT_ARGS = tuple(range(3, 11))
FLT_ARGS = tuple(range(1, 9))


class GenIndex:
    """address -> generated guest body (normal, original or fast entry)."""

    def __init__(self, gen_dir, cache=None):
        self.gen_dir = gen_dir
        cache = cache or os.path.join(gen_dir, "..", "verify", "genindex.pkl")
        stamp = max(os.path.getmtime(os.path.join(gen_dir, f)) for f in os.listdir(gen_dir) if f.startswith("code_"))
        if os.path.exists(cache) and os.path.getmtime(cache) > stamp:
            with open(cache, "rb") as f:
                self.loc = pickle.load(f)
        else:
            self.loc = {}
            for fn in sorted(os.listdir(gen_dir)):
                if not re.match(r"code_\d+\.c$", fn):
                    continue
                path = os.path.join(gen_dir, fn)
                with open(path) as f:
                    off = 0
                    start = None
                    name = None
                    for line in f:
                        m = FUNC_RE.match(line.rstrip("\n"))
                        if m:
                            start, name = off, m.group(1)
                        elif line == "}\n" and start is not None:
                            addr = int(name[2:10], 16)
                            orig = name.endswith("_orig")
                            # hooked functions: keep the game's body (f_X_orig)
                            if orig or addr not in self.loc:
                                self.loc[addr] = (fn, start, off + len(line), orig)
                            start = None
                        off += len(line)
            os.makedirs(os.path.dirname(cache), exist_ok=True)
            with open(cache, "wb") as f:
                pickle.dump(self.loc, f)
        self.addrs = sorted(self.loc)
        self._text = {}

    def text(self, addr):
        if addr not in self._text:
            fn, a, b, _ = self.loc[addr]
            with open(os.path.join(self.gen_dir, fn)) as f:
                f.seek(a)
                self._text[addr] = f.read(b - a)
        return self._text[addr]

    def size(self, addr):
        import bisect
        i = bisect.bisect_right(self.addrs, addr)
        return (self.addrs[i] - addr) if i < len(self.addrs) else 0


def load_names(build):
    """address -> (demangled name, file, evidence), address -> GameCube mangled symbol"""
    names, gc = {}, {}
    p = os.path.join(build, "names.tsv")
    if os.path.exists(p):
        for line in open(p):
            f = line.rstrip("\n").split("\t")
            if len(f) >= 4 and re.match(r"[0-9A-F]{8}$", f[0]):
                names[int(f[0], 16)] = (f[1], f[2], f[3])
    p = os.path.join(build, "wwhd_to_gc.tsv")
    if os.path.exists(p):
        for line in open(p):
            f = line.rstrip("\n").split("\t")
            if len(f) >= 3 and re.match(r"[0-9A-F]{8}$", f[0]):
                gc[int(f[0], 16)] = (f[1], f[2])
    return names, gc


# ---------------------------------------------------------------- CodeWarrior demangling (types only)

BASIC = {"v": ("void", 0), "b": ("bool", 1), "c": ("char", 1), "s": ("short", 2), "i": ("int", 4), "l": ("long", 4),
         "x": ("longlong", 8), "f": ("float", 4), "d": ("double", 8), "w": ("wchar", 2), "e": ("...", 0)}
KNOWN_SIZE = {"cXyz": 12, "Vec": 12, "csXyz": 6, "SVec": 6, "cSAngle": 2, "Quaternion": 16, "GXColor": 4,
              "_GXColor": 4, "cXy": 8, "J3DTransformInfo": 0x20}


class Type:
    def __init__(self, kind, name="", size=0, const=False, inner=None):
        self.kind, self.name, self.size, self.const, self.inner = kind, name, size, const, inner

    def __repr__(self):
        if self.kind in ("ptr", "ref"):
            return "%s%s%s" % ("const " if self.inner and self.inner.const else "", self.inner, "*" if self.kind == "ptr" else "&")
        return self.name


class Demangler:
    def __init__(self, s):
        self.s, self.i = s, 0

    def peek(self):
        return self.s[self.i] if self.i < len(self.s) else ""

    def num(self):
        j = self.i
        while self.peek().isdigit():
            self.i += 1
        return int(self.s[j:self.i])

    def name(self):
        n = self.num()
        r = self.s[self.i:self.i + n]
        self.i += n
        return r

    def qual(self):
        if self.peek() == "Q":
            self.i += 1
            k = int(self.s[self.i])
            self.i += 1
            return "::".join(self.name() for _ in range(k))
        return self.name()

    def type(self):
        const = False
        while self.peek() in ("C", "V"):
            const |= self.peek() == "C"
            self.i += 1
        ch = self.peek()
        if ch in ("P", "R"):
            self.i += 1
            inner = self.type()
            return Type("ptr" if ch == "P" else "ref", size=4, const=const, inner=inner)
        if ch == "U" or ch == "S":
            self.i += 1
            b = self.peek()
            self.i += 1
            t = BASIC[b]
            return Type("int", ("u" if ch == "U" else "s") + t[0], t[1], const)
        if ch in BASIC:
            self.i += 1
            t = BASIC[ch]
            kind = "float" if ch in "fd" else ("void" if ch == "v" else ("vararg" if ch == "e" else "int"))
            return Type(kind, t[0], t[1], const)
        if ch.isdigit() or ch == "Q":
            n = self.qual()
            return Type("class", n, KNOWN_SIZE.get(n, 0), const)
        if ch == "A":  # array: A<n>_<type>
            self.i += 1
            n = self.num()
            assert self.peek() == "_"
            self.i += 1
            t = self.type()
            return Type("array", "%s[%d]" % (t.name, n), n * (t.size or 0), const, t)
        if ch == "F":  # function type F<params>_<ret>
            self.i += 1
            while self.peek() and self.peek() != "_":
                self.type()
            self.i += 1
            self.type()
            return Type("func", "fn", 4, const)
        if ch == "M":  # pointer to member: M<class><type>
            self.i += 1
            self.type()
            self.type()
            return Type("memptr", "memptr", 12, const)
        raise ValueError("cannot demangle at %r" % self.s[self.i:])


def signature(sym):
    """GameCube mangled symbol -> (is_method, [Type]) or None. The name itself may contain
    "__" (mDoExt_J3DModel__create__FP12J3DModelDataUlUl): the first split that parses wins."""
    pos = [m.start() for m in re.finditer("__", sym) if m.start() > 0]
    for p in pos:
        r = _signature_at(sym[p + 2:])
        if r is not None:
            return r
    return None


def _signature_at(rest):
    if not rest or not (rest[0].isdigit() or rest[0] in "QF"):
        return None
    try:
        d = Demangler(rest)
        method = False
        if d.peek() != "F":
            d.qual()
            method = True
            if d.peek() == "C":
                d.i += 1
        if d.peek() != "F":
            return None
        d.i += 1
        params = []
        while d.i < len(d.s) and d.peek() != "_":
            t = d.type()
            if t.kind == "void" and not params:
                break
            params.append(t)
        if d.i != len(d.s):
            return None
        return method, params
    except Exception:
        return None


def stack_words(method, params):
    """argument words the PowerPC EABI passes on the stack (integers beyond r10)"""
    gi = 3 + (1 if method else 0)
    n = 0
    for t in params:
        if t.kind in ("float", "vararg"):
            if t.kind == "vararg":
                break
            continue
        if gi > 10:
            n += 1
        gi += 1
    return n


def arg_regs(method, params):
    """(int arg registers, float arg registers, per-register Type)"""
    ints, flts, types = [], [], {}
    gi, fi = 3, 1
    if method:
        ints.append(gi)
        types["r%d" % gi] = Type("ptr", "this", 4, inner=Type("class", "this"))
        gi += 1
    for t in params:
        if t.kind == "float":
            if fi <= 8:
                flts.append(fi)
                types["f%d" % fi] = t
            fi += 1
        elif t.kind == "vararg":
            break
        elif t.name in ("longlong", "ulonglong", "slonglong"):
            gi += gi % 2 == 0  # aligned register pair r3:r4, r5:r6, ...
            for _ in range(2):
                if gi <= 10:
                    ints.append(gi)
                    types["r%d" % gi] = t
                gi += 1
        else:
            if gi <= 10:
                ints.append(gi)
                types["r%d" % gi] = t
            gi += 1
    return ints, flts, types


# ---------------------------------------------------------------- register dataflow over the generated C

STMT_SPLIT = re.compile(r"/\* ([0-9A-F]{8}): [0-9A-F]{8} \*/$")
REG_RE = re.compile(r"c->(r|f)\[(\d+)\](\.ps[01])?(\s*=(?!=))?")
ARGS = {("r", i) for i in INT_ARGS} | {("f", i) for i in FLT_ARGS}
VOLATILE = {("r", i) for i in [0] + list(range(3, 13))} | {("f", i) for i in range(0, 14)}


class Cfg:
    """Instructions of one generated function: own register uses/defs, calls, successors,
    reachability from the entry."""

    def __init__(self, text):
        insns, labels = [], {}
        for ln in text.split("\n"):
            m = STMT_SPLIT.search(ln)
            if m:
                insns.append((int(m.group(1), 16), ln[:m.start()].strip()))
            elif ln.startswith("L_"):
                labels[int(ln[2:10], 16)] = len(insns)
        self.insns = insns
        n = len(insns)
        self.use, self.defs, self.succ, self.calls, self.indirect = [], [], [], [], []
        for k, (a, s) in enumerate(insns):
            u, d = set(), set()
            for piece in s.split(";"):  # C order: a piece's right-hand side is read before its target is written
                pd = set()
                for m in REG_RE.finditer(piece):
                    reg = (m.group(1), int(m.group(2)))
                    if m.group(4):
                        pd.add(reg)
                    elif reg not in d and m.group(3) != ".ps1":  # arguments are passed in ps0
                        u.add(reg)
                # paired-single loads/stores name the FPR as a number
                for m in re.finditer(r"psq_store\(c, (\d+),", piece):
                    if ("f", int(m.group(1))) not in d:
                        u.add(("f", int(m.group(1))))
                for m in re.finditer(r"psq_load\(c, (\d+),", piece):
                    pd.add(("f", int(m.group(1))))
                d |= pd
            self.calls.append([int(cm.group(1), 16) for cm in CALL_RE.finditer(s)])
            self.indirect.append("ppc_dispatch(c)" in s or "ppc_dispatch_cached(c," in s or
                                 bool(re.search(r"\bimp_\w+\(c\)", s)))
            nx = []
            for gm in re.finditer(r"goto L_([0-9A-F]{8})", s):
                t = int(gm.group(1), 16)
                if t in labels:
                    nx.append(labels[t])
            term = s.startswith(("return;", "MUSTTAIL return", "goto ", "switch")) or (s.startswith("c->pc =") and "MUSTTAIL" in s)
            if not term and k + 1 < n:
                nx.append(k + 1)
            self.use.append(u)
            self.defs.append(d)
            self.succ.append(nx)
        # variadic functions save r3-r10 (and f1-f8) to the register save area in the prologue;
        # those stores are not uses of the arguments
        spill = re.compile(r"^(?:\{ uint32_t ea = c->r\[1\] \+ 0x[0-9A-F]+u; stf64\(ea, c->f\[(\d)\]\.ps0\); \}|st32\(c->r\[1\] \+ 0x[0-9A-F]+u, c->r\[(\d+)\]\);)$")
        spilled = {}
        for k, (a, s) in enumerate(insns[:40]):
            m = spill.match(s)
            if m:
                reg = ("f", int(m.group(1))) if m.group(1) else ("r", int(m.group(2)))
                if reg in ARGS:
                    spilled[k] = reg
        self.varargs = len({r for r in spilled.values() if r[0] == "r"}) >= 6
        if self.varargs:
            for k, reg in spilled.items():
                self.use[k].discard(reg)
        self.reach = set()
        st = [0] if n else []
        while st:
            k = st.pop()
            if k in self.reach:
                continue
            self.reach.add(k)
            st.extend(self.succ[k])


class Dataflow:
    """Interprocedural register facts, memoised per function.

    livein(f): argument registers (r3-r10, f1-f8) f may read before writing them. A direct call
    reads the callee's live-in set; indirect calls and imports are assumed to read all argument
    registers.
    maydef(f): volatile registers (r0, r3-r12, f0-f13) f may write, transitively. GHS allocates
    registers across calls (interprocedural): a caller may keep a value in a volatile register
    across a call to a callee known not to touch it, so a mocked callee must only write
    registers the real callee may write."""

    def __init__(self, gen, unknown_reads_args=True):
        import sys
        sys.setrecursionlimit(100000)
        self.gen = gen
        self.unknown_reads_args = unknown_reads_args
        self.cfgs = {}
        self.li, self.md = {}, {}
        self.busy_li, self.busy_md = set(), set()

    def cfg(self, addr):
        if addr not in self.cfgs:
            self.cfgs[addr] = Cfg(self.gen.text(addr))
        return self.cfgs[addr]

    def maydef(self, addr):
        if addr in self.md:
            return self.md[addr]
        if addr not in self.gen.loc:
            return set(VOLATILE)
        if addr in self.busy_md:
            return set()
        self.busy_md.add(addr)
        g = self.cfg(addr)
        r = set()
        for k in g.reach:
            r |= g.defs[k]
            for t in g.calls[k]:
                r |= self.maydef(t)
            if g.indirect[k]:
                r |= VOLATILE
        self.busy_md.discard(addr)
        r &= VOLATILE
        self.md[addr] = r
        return r

    def livein(self, addr):
        if addr in self.li:
            return self.li[addr]
        if addr not in self.gen.loc or addr in self.busy_li:
            return set()
        self.busy_li.add(addr)
        g = self.cfg(addr)
        n = len(g.insns)
        use, defs = [], []
        for k in range(n):
            u, d = set(g.use[k]), set(g.defs[k])
            if k in g.reach:
                for t in g.calls[k]:
                    u |= self.livein(t) - d
                    d |= self.maydef(t)
                if g.indirect[k]:
                    if self.unknown_reads_args:
                        u |= ARGS - d
                    d |= VOLATILE
            use.append(u)
            defs.append(d)
        live = [set() for _ in range(n)]
        changed = True
        while changed:
            changed = False
            for k in range(n - 1, -1, -1):
                out = set()
                for j in g.succ[k]:
                    out |= live[j]
                new = use[k] | (out - defs[k])
                if new != live[k]:
                    live[k] = new
                    changed = True
        self.busy_li.discard(addr)
        r = (live[0] & ARGS) if n else set()
        self.li[addr] = r
        return r


# ---------------------------------------------------------------- return value width

ASSIGN_RE = re.compile(r"^\{?\s*c->r\[(\d+)\] = (.*)$")


def classify_def(rhs):
    """value range of `c->r[N] = rhs` -> ('u', bits) / ('s', bits) / ('reg', n) / ('const', v) / None (full)"""
    rhs = rhs.strip().rstrip(";").strip()
    m = re.match(r"^c->r\[(\d+)\]$", rhs)
    if m:
        return ("reg", int(m.group(1)))
    if re.match(r"^ld8\(", rhs):
        return ("u", 8)
    if re.match(r"^ld16\(", rhs):
        return ("u", 16)
    if "(int16_t)" in rhs and rhs.startswith("(uint32_t)(int32_t)(int16_t)"):
        return ("s", 16)
    if rhs.startswith("(uint32_t)(int32_t)(int8_t)"):
        return ("s", 8)
    m = re.match(r"^0u \+ 0x([0-9A-F]{8})u$", rhs)
    if m:
        return ("const", int(m.group(1), 16))
    m = re.search(r"& 0x([0-9A-F]{4,8})u(?:; cr0_rc.*)?$", rhs)
    if m and not re.search(r"\|", rhs):
        mask = int(m.group(1), 16)
        if mask and (mask & (mask + 1)) == 0:  # low-bit mask
            return ("u", mask.bit_length())
    if re.match(r"^c->r\[\d+\] \? \(uint32_t\)__builtin_clz", rhs):
        return ("u", 6)
    return None


def merge_width(a, b):
    if a is None or b is None:
        return None
    if a == b:
        return a
    if a[0] == "const":
        a, b = b, a
    if b[0] == "const":
        v = b[1]
        if a[0] == "const":
            return merge_width(("u", max(1, a[1].bit_length())) if a[1] < 0x80000000 else None, b)
        if a[0] == "u" and v < (1 << a[1]):
            return a
        if a[0] == "s" and (v < (1 << (a[1] - 1)) or v >= 0x100000000 - (1 << (a[1] - 1))):
            return a
        if a[0] == "u" and v < 0x80000000:
            return ("u", max(a[1], v.bit_length()))
        return None
    if a[0] == b[0] == "u":
        return ("u", max(a[1], b[1]))
    if a[0] == b[0] == "s":
        return ("s", max(a[1], b[1]))
    return None


class RetWidth:
    """How a function's r3 result is extended at every return: ('u', bits) zero-extended,
    ('s', bits) sign-extended, ('const', v), or None (any 32-bit value). GHS callers rely on it
    (a u8 result is used without re-extension), so mocks must return values of that shape."""

    def __init__(self, df):
        self.df = df
        self.memo = {}
        self.busy = set()

    def width(self, addr):
        if addr in self.memo:
            return self.memo[addr]
        if addr not in self.df.gen.loc or addr in self.busy:
            return None
        self.busy.add(addr)
        g = self.df.cfg(addr)
        preds = [[] for _ in g.insns]
        for k, ss in enumerate(g.succ):
            for j in ss:
                preds[j].append(k)
        result = "none"
        for k in g.reach:
            s = g.insns[k][1]
            if s.startswith("return;") or (s.startswith("if (") and "return;" in s):
                w = self._trace(g, preds, k, 3, set())
                result = w if result == "none" else merge_width(result, w)
                if result is None:
                    break
            elif s.startswith("MUSTTAIL return f_"):
                t = int(s[len("MUSTTAIL return f_"):][:8], 16)
                w = self.width(t)
                result = w if result == "none" else merge_width(result, w)
                if result is None:
                    break
        self.busy.discard(addr)
        r = None if result == "none" else result
        self.memo[addr] = r
        return r

    def _trace(self, g, preds, k, reg, seen):
        """range of register `reg` just before instruction k (all paths)"""
        res = "none"
        work = [(j, reg) for j in preds[k]]
        if k == 0 or not preds[k]:
            return None
        while work:
            j, r = work.pop()
            if (j, r) in seen:
                continue
            seen.add((j, r))
            s = g.insns[j][1]
            w = "pass"
            for piece in reversed(s.split(";")):
                m = ASSIGN_RE.match(piece.strip())
                if m and int(m.group(1)) == r:
                    w = classify_def(m.group(2))
                    break
            else:
                for t in g.calls[j]:
                    if ("r", r) in self.df.maydef(t):
                        w = self.width(t) if r == 3 else None
                        break
                if w == "pass" and g.indirect[j]:
                    w = None
            if w == "pass":
                if j == 0 or not preds[j]:
                    return None  # value comes from the caller
                work.extend((p, r) for p in preds[j])
                continue
            if w is not None and w[0] == "reg":
                if j == 0 or not preds[j]:
                    return None
                work.extend((p, w[1]) for p in preds[j])
                continue
            res = w if res == "none" else merge_width(res, w)
            if res is None:
                return None
        return None if res == "none" else res
