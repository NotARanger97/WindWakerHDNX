"""Translate Espresso PowerPC instructions to C statements.

Each instruction becomes a C statement operating on `Cpu* c` (see
runtime/include/ppc.h). Control flow is handled by the caller through the
Ctx callbacks: `branch(target)` returns C for a jump, `call(target)` for a call.

Semantics follow Cemu's interpreter (src/Cafe/HW/Espresso/Interpreter).
"""
import re


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def mask(mb, me):
    """PowerPC MASK(mb, me) with bit 0 = MSB."""
    m = 0
    i = mb
    while True:
        m |= 1 << (31 - i)
        if i == me:
            break
        i = (i + 1) & 31
    return m


class Unhandled(Exception):
    pass


def custom_abi(name):
    """Names whose register/context contract is not an ordinary EABI call."""
    return bool(re.search(r"(?:setjmp|longjmp|savegpr|restgpr|savefpr|restfpr|"
                          r"OSSwitch(?:Fiber|Stack)|__OSSwitchStack|"
                          r"OS(?:Load|Save|Set|Get)Context|"
                          r"OS\w*(?:Thread|Mutex|Semaphore|Cond|Event|Message))", name, re.I))


def cpu_calls(src):
    """Calls with c as their first argument in our emitted C (not arbitrary C).

    Arguments may contain casts/nested calls, so do not split them with a regex.
    The emitter has no string literals in these arguments.
    """
    for m in re.finditer(r"\b(\w+)\(c(?=[,)])", src):
        depth, begin, args = 1, m.end(1) + 1, []
        for i in range(begin, len(src)):
            ch = src[i]
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    args.append(src[begin:i].strip())
                    yield m.start(), i + 1, m[1], args
                    break
            elif ch == "," and depth == 1:
                args.append(src[begin:i].strip())
                begin = i + 1
        else:
            raise ValueError("unterminated CPU helper: " + src)


def expand_cr_helpers(src):
    """Spell out CR helpers so compares/record instructions update C locals.

    Keep this equivalent to ppc.h, including byte truncation, unordered compares,
    FPSCR updates and mfcr's masking of bits. Other Cpu fields stay in memory.
    """
    def compare(field, x, y, kind):
        bits = ["c->cr[%d]" % (4 * field + i) for i in range(4)]
        typ = {"s": "int32_t", "u": "uint32_t", "f": "double"}[kind]
        pre = "int cr_un = __builtin_isunordered(cr_a, cr_b); " if kind == "f" else ""
        # Ordered C relations already return false for unordered operands.
        # Let one FP comparison supply all four results instead of guarding
        # each relation with two independent isnan comparisons.
        result = " ".join("%s = cr_a %s cr_b;" % (bit, op)
                          for bit, op in zip(bits, ("<", ">", "==")))
        result += " %s = %s;" % (bits[3], "(uint8_t)cr_un" if kind == "f" else "c->xer_so")
        if kind == "f":
            result += (" c->fpscr = (c->fpscr & ~0xF000u) | "
                       "((uint32_t)(%s << 3 | %s << 2 | %s << 1 | cr_un) << 12);") % tuple(bits[:3])
        return "{ %s cr_a = %s, cr_b = %s; %s%s }" % (typ, x, y, pre, result)

    pieces, pos = [], 0
    for start, end, name, args in cpu_calls(src):
        replacement = None
        if name in ("cr_set_s", "cr_set_u", "cr_set_f"):
            replacement = compare(int(args[1]), args[2], args[3], name[-1])
        elif name == "cr0_rc":
            replacement = compare(0, "(int32_t)(%s)" % args[1], "0", "s")
        elif name == "ppc_mfcr":
            replacement = "(" + " | ".join("((uint32_t)(c->cr[%d] & 1) << %d)" % (i, 31 - i)
                                             for i in range(32)) + ")"
        elif name == "ppc_mtcrf":
            crm = int(args[1], 0)
            bits = [i for i in range(32) if crm & (0x80 >> (i // 4))]
            replacement = "{ uint32_t cr_v = %s; %s }" % (args[2], " ".join(
                "c->cr[%d] = (cr_v >> %d) & 1;" % (i, 31 - i) for i in bits))
        if replacement is not None:
            pieces += [src[pos:start], replacement]
            pos = end
    pieces.append(src[pos:])
    src = "".join(pieces)
    # mcrf copies either identical or disjoint four-byte fields; no overlap.
    return re.sub(r"memmove\(&c->cr\[(\d+)\], &c->cr\[(\d+)\], 4\)", lambda m:
                  "{ " + " ".join("c->cr[%d] = c->cr[%d];" % (int(m[1]) + i, int(m[2]) + i)
                                    for i in range(4)) + " }", src)


def leaf_inputs(body, outputs, import_inputs, import_outputs):
    """Upward-exposed cached fields of an already proven guest leaf.

    Outputs are live at every exit: a conditional write must retain the entry
    value on its bypass path. Conditional instructions have no definite kills.
    Calls here can only be audited ordinary imports (eligibility is separate).
    """
    by_addr = {a: i for i, (a, _, _) in enumerate(body)}
    successors, uses, kills = [], [], []
    for i, (a, w, src) in enumerate(body):
        src = expand_cr_helpers(src)
        u, d = set(), set()
        for m in RegisterLocals.field.finditer(src):
            if RegisterLocals.write.match(src, m.end()):
                d.add(m[0])
            if not re.match(r"\s*=(?!=)", src[m.end():]):
                u.add(m[0])
        for _, _, name, _ in cpu_calls(src):
            if name.startswith("imp_"):
                u.update(import_inputs)
                d.update(import_outputs)
            elif name == "ppc_stwcx":
                d.update("c->cr[%d]" % k for k in range(4))
        edges = {by_addr[int(m[1], 16)] for m in re.finditer(r"goto L_([0-9A-F]{8});", src)}
        op, xo, bo = w >> 26, (w >> 1) & 1023, (w >> 21) & 31
        fall = not (op == 18 or (op == 16 and (bo & 20) == 20) or
                    (op == 19 and xo in (16, 528) and (bo & 20) == 20)) or bool(w & 1)
        if fall:
            edges.add(i + 1 if i + 1 < len(body) else len(body))
        if re.search(r"\breturn;", src):
            edges.add(len(body))
        successors.append(edges)
        uses.append(u)
        kills.append(set() if re.search(r"\b(?:if|switch)\s*\(", src) else d)
    live = [set() for _ in body] + [set(outputs)]
    changed = True
    while changed:
        changed = False
        for i in range(len(body) - 1, -1, -1):
            before = uses[i] | (set().union(*(live[j] for j in successors[i])) - kills[i])
            if before != live[i]:
                live[i] = before
                changed = True
    return frozenset(live[0])


def dead_flags(body, sites=()):
    """Drop only flag stores overwritten on every path before an observation.

    All flags are live at exits and guest/host calls. FPSCR's compare nibble
    (FPRF) is tracked separately: its masked update does not read the old FPRF.
    Unrecognized helpers/stores and conditional writes conservatively keep it.
    Guest memory transfers, FP arithmetic and sticky SO updates are unchanged.
    """
    field = re.compile(r"c->(?:cr\[\d+\]|xer_(?:ca|ov|so)\b)")
    fprf = re.compile(r"c->fpscr = \(c->fpscr & ~0xF000u\) \| [^;]+;")
    store = re.compile(r"(c->(?:cr\[\d+\]|xer_(?:ca|ov|so))) = ([^;]+);")
    all_flags = {"c->cr[%d]" % i for i in range(32)} | {
        "c->xer_ca", "c->xer_ov", "c->xer_so", "fprf"}
    by_addr = {a: i for i, (a, _, _) in enumerate(body)}
    sources = [expand_cr_helpers(src) for _, _, src in body]
    successors = []
    for i, (a, w, _) in enumerate(body):
        edges = {by_addr[int(m[1], 16)] for m in re.finditer(r"goto L_([0-9A-F]{8});", sources[i])
                 if int(m[1], 16) in by_addr}
        op, xo, bo = w >> 26, (w >> 1) & 1023, (w >> 21) & 31
        fall = not (op == 18 or (op == 16 and (bo & 20) == 20) or
                    (op == 19 and xo in (16, 528) and (bo & 20) == 20)) or bool(w & 1)
        if fall:
            edges.add(i + 1)
        if re.search(r"\breturn;", sources[i]):
            edges.add(len(body))
        successors.append(edges)
    # Removing an FPRF update can expose its CR inputs as dead as well.
    changed = True
    while changed:
        uses, kills = [], []
        for (a, _, _), src in zip(body, sources):
            u, d = set(), set()
            conditional = bool(re.search(r"\b(?:if|switch)\s*\(", src))
            defined_at = {}
            for m in field.finditer(src):
                if re.match(r"\s*=(?!=)", src[m.end():]):
                    d.add(m[0])
                    # A definition takes effect after its RHS, not at the LHS:
                    # self-reads still consume the incoming value. Straight
                    # fcmp bodies, however, build FPRF from their *new* CR bits.
                    end = src.find(";", m.end())
                    if not conditional and end >= 0:
                        defined_at.setdefault(m[0], end)
                elif m.start() <= defined_at.get(m[0], len(src)):
                    u.add(m[0])
            rest, n = fprf.subn("", src)
            if n:
                d.add("fprf")
            if "c->fpscr" in rest:
                u.add("fprf")
            for _, _, name, _ in cpu_calls(src):
                if name in ("psq_load", "psq_store", "ppc_lwarx"):
                    continue
                if name == "ppc_stwcx":
                    u.add("c->xer_so")
                    d.update("c->cr[%d]" % j for j in range(4))
                else:
                    u.update(all_flags)
            if a in sites:
                u.update(all_flags)
            uses.append(u)
            kills.append(set() if conditional else d)
        live = [set() for _ in body] + [set(all_flags)]
        again = True
        while again:
            again = False
            for i in range(len(body) - 1, -1, -1):
                before = uses[i] | (set().union(*(live[j] for j in successors[i])) - kills[i])
                if before != live[i]:
                    live[i] = before
                    again = True
        changed = False
        for i, src in enumerate(sources):
            after = set().union(*(live[j] for j in successors[i]))
            # Hooks run before the instruction; calls inside an instruction may
            # observe intermediate values, so do not edit such instructions.
            if any(name not in ("psq_load", "psq_store", "ppc_lwarx")
                   for _, _, name, _ in cpu_calls(src)) or re.search(r"\b(?:if|switch)\s*\(", src):
                continue
            new = fprf.sub("", src) if "fprf" not in after else src
            # Chained assignments are left intact. Compound/sticky assignments
            # are not candidates; ordinary stores with self-reads stay too.
            new = store.sub(lambda m: "" if m[1] not in after and "=" not in m[2] and
                            m[1] not in m[2] and m[1] not in new[m.end():] else m[0], new)
            if new != src:
                sources[i] = new
                changed = True
    return [(a, w, src) for (a, w, _), src in zip(body, sources)]


def single_rounds(body, sites=()):
    """Remove round25 only from FPR lanes proven to hold promoted floats.

    A float promoted to double has at least 29 zero low significand bits,
    including infinities/NaN payloads. round25 clears 27 and adds bit 27, so it
    is exactly the identity here. This does not change FP arithmetic/rounding.
    Use a must analysis over the emitted CFG; calls/hooks forget every lane.
    """
    if not body or not any("round25(" in src for _, _, src in body):
        return body
    lane = re.compile(r"c->f\[(\d+)\]\.ps([01])")
    universe = frozenset((n, p) for n in range(32) for p in range(2))
    by_addr = {a: i for i, (a, _, _) in enumerate(body)}
    successors, predecessors = [], [set() for _ in body]
    for i, (a, w, src) in enumerate(body):
        op, xo, bo = w >> 26, (w >> 1) & 1023, (w >> 21) & 31
        edges = {by_addr[int(m[1], 16)] for m in re.finditer(r"goto L_([0-9A-F]{8});", src)
                 if int(m[1], 16) in by_addr}
        fall = True
        if op == 18 or (op == 16 and (bo & 20) == 20) or (op == 19 and xo in (16, 528) and (bo & 20) == 20):
            fall = bool(w & 1)
        if fall and i + 1 < len(body):
            edges.add(i + 1)
        successors.append(edges)
        for j in edges:
            predecessors[j].add(i)
    reachable, pending = set(), [0]
    while pending:
        i = pending.pop()
        if i not in reachable:
            reachable.add(i)
            pending.extend(successors[i])

    def transfer(i, incoming):
        a, w, src = body[i]
        known = set() if a in sites else set(incoming)
        if any(RegisterLocals.observer.fullmatch(name) for _, _, name, _ in cpu_calls(src)):
            return frozenset()
        old = set(known)
        # Unknown/new opcodes cannot accidentally inherit a destination's tag.
        for m in lane.finditer(src):
            if RegisterLocals.write.match(src, m.end()):
                known.discard((int(m[1]), int(m[2])))
        op, d, b, cc = w >> 26, (w >> 21) & 31, (w >> 11) & 31, (w >> 6) & 31
        a_reg, xo5, xo = (w >> 16) & 31, (w >> 1) & 31, (w >> 1) & 1023
        pair = ((d, 0), (d, 1))
        copies = {}
        if op in (48, 49, 56, 57, 59) or (op == 63 and xo == 12):
            known.update(pair)
        elif op == 63 and xo in (40, 72, 136, 264):
            copies[(d, 0)] = (b, 0)
        elif op == 4:
            if xo5 == 6 or xo5 in (12, 13, 14, 15, 18, 20, 21, 24, 25, 26, 28, 29, 30, 31):
                known.update(pair)
            elif xo5 == 10:
                known.add((d, 0))
                copies[(d, 1)] = (cc, 1)
            elif xo5 == 11:
                copies[(d, 0)] = (cc, 0)
                known.add((d, 1))
            elif xo5 == 23:
                for p in (0, 1):
                    if (b, p) in old and (cc, p) in old:
                        known.add((d, p))
            elif xo in (40, 72, 136, 264):
                copies = {(d, p): (b, p) for p in (0, 1)}
            elif xo in (528, 560, 592, 624):
                p0, p1 = {528: (0, 0), 560: (0, 1), 592: (1, 0), 624: (1, 1)}[xo]
                copies = {(d, 0): (a_reg, p0), (d, 1): (b, p1)}
        for dest, source in copies.items():
            if source in old:
                known.add(dest)
        return frozenset(known)

    incoming, outgoing = [universe for _ in body], [universe for _ in body]
    changed = True
    while changed:
        changed = False
        for i in sorted(reachable):
            preds = predecessors[i] & reachable
            before = frozenset.intersection(*(outgoing[j] for j in preds)) if preds else frozenset()
            if i == 0 or body[i][0] in sites:
                before = frozenset()
            after = transfer(i, before)
            if before != incoming[i] or after != outgoing[i]:
                incoming[i], outgoing[i] = before, after
                changed = True
    result = []
    pattern = re.compile(r"round25\((c->f\[(\d+)\]\.ps([01]))\)")
    for i, (a, w, src) in enumerate(body):
        known = incoming[i] if i in reachable else frozenset()
        src = pattern.sub(lambda m: m[1] if (int(m[2]), int(m[3])) in known else m[0], src)
        result.append((a, w, src))
    return result


class RegisterLocals:
    """Function-wide cache, with ABI barriers only for audited ordinary calls.

    All touched fields start initialized, including write-only fields: a goto or
    conditional write may bypass any definition. CFG dataflow narrows full
    barriers to dirty stores and live reloads. Ordinary imports use the EABI
    contract; proven guest leaves use actual access/output summaries, including
    non-volatile outputs. Without dataflow, barriers remain function-wide.
    """
    field = re.compile(r"c->(?:r\[\d+\]|cr\[\d+\]|lr\b|ctr\b)")
    write = re.compile(r"\s*(?:=(?!=)|[+\-*/&|^]=|\+\+|--)")
    # Audited against ppc.h: these access only uncached fields (FPR/GQR,
    # reservation, FPSCR, XER), never GPR/CR/LR/CTR. Unknown helpers fail closed.
    uncached_helpers = {"psq_load", "psq_store", "ppc_lwarx", "ppc_fctiw",
                        "ppc_mfxer", "ppc_mtxer"}
    observer = re.compile(r"(?:f_|imp_|hook_|site_)\w+\Z|ppc_(?:dispatch(?:_cached)?|unimplemented|trap)\Z")

    @staticmethod
    def volatile(field):
        if field in ("c->lr", "c->ctr"):
            return True
        n = int(field[field.index("[") + 1:-1])
        return (n < 8 or n >= 20) if "->cr[" in field else (n == 0 or 3 <= n <= 12)

    @staticmethod
    def nonvolatile(field):
        return not RegisterLocals.volatile(field) and field not in (
            "c->r[1]", "c->r[2]", "c->r[13]")

    def __init__(self, statements, abi=True, abi_calls=(), pack_cr_stores=True, call_summaries=None):
        self.abi = abi
        self.pack_cr_stores = pack_cr_stores
        self.abi_calls = frozenset(abi_calls)
        self.call_summaries = call_summaries or {}
        self.statements = [expand_cr_helpers(s) for s in statements]
        touched, written = set(), set()
        for src in self.statements:
            for m in self.field.finditer(src):
                touched.add(m[0])
                if self.write.match(src, m.end()):
                    written.add(m[0])
            if any(name == "ppc_stwcx" for _, _, name, _ in cpu_calls(src)):
                written.update("c->cr[%d]" % i for i in range(4))
        touched |= written
        self.fields = sorted(touched)
        self.written = sorted(written)
        self.call_written = [f for f in self.written if not self.nonvolatile(f)]
        self.call_fields = [f for f in self.fields if self.volatile(f)]
        self.sync_dirty = self.sync_live = None

    def call_effect(self, name, tail=False):
        """Fields made coherent by a call, and fields it may overwrite."""
        summary = self.call_summaries.get(name) if self.abi and not tail else None
        if summary is not None:
            return summary
        if self.abi and not tail and (name in self.abi_calls or
                                     (name.startswith("imp_") and not custom_abi(name))):
            return (set(self.call_written), set(self.call_fields))
        return (set(self.fields), set(self.fields))

    def analyze_sync(self, successors):
        """May-dirty / live-local analysis over the instruction CFG.

        A full observation makes every local coherent with Cpu; a partial call
        does so only for flushed/reloaded fields. Return stores need only fields
        dirtied since that observation. Liveness then removes reloads with no
        subsequent local read (including reads by a later dirty flush).
        Conditional writes/calls do not kill incoming values. This deliberately
        over-approximates multi-statement instruction bodies.
        """
        n = len(self.statements)
        uses, writes, kills, calls, returns, conditional = [], [], [], [], [], []
        for src in self.statements:
            u, d = set(), set()
            for m in self.field.finditer(src):
                writing = self.write.match(src, m.end())
                if writing:
                    d.add(m[0])
                if not re.match(r"\s*=(?!=)", src[m.end():]):
                    u.add(m[0])
            cond = bool(re.search(r"\b(?:if|switch)\s*\(", src))
            observed = [(name, "MUSTTAIL return " + name + "(" in src)
                        for _, _, name, _ in cpu_calls(src) if self.observer.fullmatch(name)]
            if any(name == "ppc_stwcx" for _, _, name, _ in cpu_calls(src)):
                # The helper writes Cpu directly and its local reload is coherent.
                d.update("c->cr[%d]" % i for i in range(4))
            uses.append(u)
            writes.append(d)
            kills.append(set() if cond else d)
            calls.append(observed)
            returns.append(bool(re.search(r"\breturn;", src)))
            conditional.append(cond)
        predecessors = [set() for _ in range(n)]
        for i, edges in enumerate(successors):
            for j in edges:
                predecessors[j].add(i)
        reachable, pending = set(), [0]
        while pending:
            i = pending.pop()
            if i not in reachable:
                reachable.add(i)
                pending.extend(successors[i])
        incoming, outgoing = [set() for _ in range(n)], [set() for _ in range(n)]
        changed = True
        while changed:
            changed = False
            for i in sorted(reachable):
                before = set().union(*(outgoing[j] for j in predecessors[i] & reachable))
                after = before | writes[i]
                if not conditional[i]:
                    for name, tail in calls[i]:
                        accessed, modified = self.call_effect(name, tail)
                        after -= set(accessed) | set(modified)
                    if "ppc_stwcx(c," in self.statements[i]:
                        after -= {"c->cr[%d]" % k for k in range(4)}
                if before != incoming[i] or after != outgoing[i]:
                    incoming[i], outgoing[i] = before, after
                    changed = True
        self.sync_dirty = [incoming[i] | writes[i] for i in range(n)]
        live_in, live_out = [set() for _ in range(n)], [set() for _ in range(n)]
        changed = True
        while changed:
            changed = False
            for i in sorted(reachable, reverse=True):
                after = set().union(*(live_in[j] for j in successors[i]))
                u, d = set(uses[i]), set(kills[i])
                dirty = self.sync_dirty[i]
                if returns[i]:
                    u |= dirty
                for name, tail in calls[i]:
                    accessed, modified = self.call_effect(name, tail)
                    u |= dirty & set(accessed)
                    if re.fullmatch(r"f_[0-9A-F]{8}_(?:abi|sync)", name) and not tail:
                        u |= dirty  # caller-side preemption's full flush
                    if not conditional[i]:
                        d |= set(modified)
                before = u | (after - d)
                if before != live_in[i] or after != live_out[i]:
                    live_in[i], live_out[i] = before, after
                    changed = True
        self.sync_live = live_out

    @staticmethod
    def local(field):
        return field[3:].replace("[", "").replace("]", "")

    def declarations(self):
        fields, out = set(self.fields), []
        for first, group in self.cr_groups(fields):
            out.append("uint32_t cr_init%d = ppc_cr_load_word(c, %d);" % (first, first))
            out.extend("uint8_t %s = (uint8_t)(cr_init%d >> %d);" % (self.local(field), first, 8 * b)
                       for b, field in enumerate(group))
            fields.difference_update(group)
        out.extend("%s %s = %s;" % ("uint8_t" if "->cr[" in field else "uint32_t",
                                    self.local(field), field) for field in sorted(fields))
        return out

    @staticmethod
    def cr_groups(fields):
        for first in range(0, 32, 4):
            group = ["c->cr[%d]" % (first + b) for b in range(4)]
            if all(field in fields for field in group):
                yield first, group

    def flush(self, fields=None):
        fields = set(self.written if fields is None else fields)
        stores = []
        for first, group in self.cr_groups(fields) if self.pack_cr_stores else ():
            stores.append("ppc_cr_store_word(c, %d, %s);" % (first,
                " | ".join("((uint32_t)%s << %d)" % (self.local(field), 8 * b)
                           for b, field in enumerate(group))))
            fields.difference_update(group)
        stores.extend("%s = %s;" % (field, self.local(field)) for field in sorted(fields))
        return " ".join(stores)

    def reload(self, fields=None):
        fields, loads = set(self.fields if fields is None else fields), []
        for first, group in self.cr_groups(fields):
            loads.append("{ uint32_t cr_word = ppc_cr_load_word(c, %d); %s }" % (first,
                " ".join("%s = (uint8_t)(cr_word >> %d);" % (self.local(field), 8 * b)
                         for b, field in enumerate(group))))
            fields.difference_update(group)
        loads.extend("%s = %s;" % (self.local(field), field) for field in sorted(fields))
        return " ".join(loads)

    def emit(self, src, index=None):
        dirty = self.sync_dirty[index] if self.sync_dirty is not None and index is not None else set(self.written)
        live = self.sync_live[index] if self.sync_live is not None and index is not None else set(self.fields)
        src = self.field.sub(lambda m: self.local(m[0]), src)
        # Replace bare returns before inserting calls; never put anything after
        # a MUSTTAIL return, including on conditional and fallthrough exits.
        src = re.sub(r"\breturn;", "{ %s return; }" % self.flush(dirty), src)
        pieces, pos = [], 0
        for start, end, name, args in cpu_calls(src):
            # Packed stores were just inserted by the return flush above;
            # their arguments are current locals and they do not change them.
            if name in self.uncached_helpers or name == "ppc_cr_store_word":
                continue
            if src[end:end + 1] != ";":
                raise ValueError("CPU observer is not a statement: " + src)
            end += 1
            if name == "ppc_stwcx":
                # Reads XER/reservation, overwrites exactly CR0. Those CR bits
                # are now current in Cpu; never overwrite them from stale locals.
                after = self.reload({"c->cr[%d]" % i for i in range(4)} & live)
                replacement = "{ %s %s }" % (src[start:end], after)
            elif self.observer.fullmatch(name):
                tail = src[max(0, start - len("MUSTTAIL return ")):start] == "MUSTTAIL return "
                if tail:
                    start -= len("MUSTTAIL return ")
                summary = self.call_summaries.get(name) if self.abi and not tail else None
                ordinary = self.abi and not tail and (summary is not None or name in self.abi_calls or
                            (name.startswith("imp_") and not custom_abi(name)))
                accessed, modified = self.call_effect(name, tail)
                before = self.flush(dirty & set(accessed))
                after = self.reload(live & set(modified))
                # Fast guest entries skip PPC_ENTER. Perform its check here,
                # while the caller can still materialize *all* its locals.
                # Do not check again in the callee: a second preempt check could
                # observe a request arriving after the partial write-back.
                entry = ""
                if ordinary and re.fullmatch(r"f_[0-9A-F]{8}_(?:abi|sync)", name):
                    entry = ("if (__builtin_expect(PPC_TRACING, 0)) ppc_trace_enter(0x%su); "
                             "if (__builtin_expect(g_core_preempt[c->core], 0)) { %s "
                             "ppc_preempt(c); %s } ") % (name[2:10], self.flush(dirty), self.reload(live | dirty))
                replacement = "{ %s%s %s%s }" % (
                    entry, before, src[start:end], "" if tail else " " + after)
            else:
                raise ValueError("unaudited CPU helper: " + name)
            pieces += [src[pos:start], replacement]
            pos = end
        pieces.append(src[pos:])
        return "".join(pieces)


R = lambda n: "c->r[%d]" % n
F0 = lambda n: "c->f[%d].ps0" % n
F1 = lambda n: "c->f[%d].ps1" % n


def ra0(a):
    """(rA|0) operand."""
    return "0u" if a == 0 else R(a)


def cond_expr(bo, bi):
    """C condition for a bc-family branch; the CTR decrement is emitted separately."""
    parts = []
    if not (bo & 0x04):
        parts.append("c->ctr == 0" if bo & 0x02 else "c->ctr != 0")
    if not (bo & 0x10):
        parts.append("c->cr[%d]" % bi if bo & 0x08 else "!c->cr[%d]" % bi)
    return " && ".join(parts) if parts else None


def rc(w, dst_expr):
    return " cr0_rc(c, %s);" % dst_expr if w & 1 else ""


def translate(addr, w, ctx):
    """Return C source for the instruction word `w` at `addr`."""
    op = w >> 26
    d = (w >> 21) & 31
    a = (w >> 16) & 31
    b = (w >> 11) & 31
    cc = (w >> 6) & 31
    simm = sext16(w & 0xFFFF)
    uimm = w & 0xFFFF

    # --- immediate-reloc overrides (data imports) ---
    if addr in ctx.imm_override:
        value = ctx.imm_override[addr]
        # instruction forms using a relocated immediate: addi/addis/lwz etc.
        simm = sext16(value)
        uimm = value

    if op == 18:  # b / ba / bl / bla
        tgt = (sext(w & 0x03FFFFFC, 26) + (0 if w & 2 else addr)) & 0xFFFFFFFF
        if w & 1:
            return "c->lr = 0x%08Xu; %s" % (addr + 4, ctx.call(addr, tgt))
        return ctx.branch(addr, tgt)

    if op == 16:  # bc
        bo, bi = d, a
        tgt = (sext(w & 0xFFFC, 16) + (0 if w & 2 else addr)) & 0xFFFFFFFF
        pre = "" if bo & 0x04 else "c->ctr--; "
        cond = cond_expr(bo, bi)
        if w & 1:
            body = "c->lr = 0x%08Xu; %s" % (addr + 4, ctx.call(addr, tgt))
        else:
            body = ctx.branch(addr, tgt)
        return pre + ("if (%s) { %s }" % (cond, body) if cond else body)

    if op == 19:
        xo = (w >> 1) & 0x3FF
        if xo in (16, 528):  # bclr / bcctr
            bo, bi = d, a
            if xo == 528 and not (bo & 0x04):
                raise Unhandled("bcctr with ctr decrement")
            pre = "" if bo & 0x04 else "c->ctr--; "
            cond = cond_expr(bo, bi)
            src = "c->lr" if xo == 16 else "c->ctr"
            if w & 1:
                body = "{ uint32_t t = %s; c->lr = 0x%08Xu; c->pc = t; %s }" % (src, addr + 4, ctx.indirect_call(addr))
            elif xo == 16:
                body = ctx.ret()
            else:
                body = ctx.indirect_jump(addr)
            return pre + ("if (%s) { %s }" % (cond, body) if cond else body)
        crop = {257: "&", 449: "|", 193: "^"}
        if xo in crop:
            return "c->cr[%d] = c->cr[%d] %s c->cr[%d];" % (d, a, crop[xo], b)
        if xo == 225:  # crnand
            return "c->cr[%d] = !(c->cr[%d] & c->cr[%d]);" % (d, a, b)
        if xo == 33:  # crnor
            return "c->cr[%d] = !(c->cr[%d] | c->cr[%d]);" % (d, a, b)
        if xo == 289:  # creqv
            return "c->cr[%d] = !(c->cr[%d] ^ c->cr[%d]);" % (d, a, b)
        if xo == 129:  # crandc
            return "c->cr[%d] = c->cr[%d] & !c->cr[%d];" % (d, a, b)
        if xo == 417:  # crorc
            return "c->cr[%d] = c->cr[%d] | !c->cr[%d];" % (d, a, b)
        if xo == 0:  # mcrf
            fd, fs = d >> 2, a >> 2
            return "memmove(&c->cr[%d], &c->cr[%d], 4);" % (4 * fd, 4 * fs)
        if xo == 150:  # isync: orders later loads after earlier ones (acquire on Espresso's lock idioms)
            return "__atomic_thread_fence(__ATOMIC_ACQUIRE);"
        raise Unhandled("op19 xo=%d" % xo)

    # --- integer immediate ---
    if op == 14:  # addi / li
        return "%s = %s + 0x%08Xu;" % (R(d), ra0(a), simm & 0xFFFFFFFF)
    if op == 15:  # addis / lis
        return "%s = %s + 0x%08Xu;" % (R(d), ra0(a), (uimm << 16) & 0xFFFFFFFF)
    if op in (12, 13):  # addic / addic.
        s = "{ uint64_t t = (uint64_t)%s + 0x%08Xu; %s = (uint32_t)t; c->xer_ca = (uint8_t)(t >> 32); }" % (
            R(a), simm & 0xFFFFFFFF, R(d))
        return s + (" cr0_rc(c, %s);" % R(d) if op == 13 else "")
    if op == 8:  # subfic
        return "{ uint64_t t = (uint64_t)(uint32_t)~%s + 0x%08Xu + 1; %s = (uint32_t)t; c->xer_ca = (uint8_t)(t >> 32); }" % (
            R(a), simm & 0xFFFFFFFF, R(d))
    if op == 7:  # mulli
        return "%s = (uint32_t)((int32_t)%s * %d);" % (R(d), R(a), simm)
    if op == 10:  # cmpli
        return "cr_set_u(c, %d, %s, 0x%04Xu);" % (d >> 2, R(a), uimm)
    if op == 11:  # cmpi
        return "cr_set_s(c, %d, (int32_t)%s, %d);" % (d >> 2, R(a), simm)
    if op == 24:  # ori
        return ";" if (a == d == 0 and uimm == 0) else "%s = %s | 0x%04Xu;" % (R(a), R(d), uimm)
    if op == 25:
        return "%s = %s | 0x%08Xu;" % (R(a), R(d), uimm << 16)
    if op == 26:
        return "%s = %s ^ 0x%04Xu;" % (R(a), R(d), uimm)
    if op == 27:
        return "%s = %s ^ 0x%08Xu;" % (R(a), R(d), uimm << 16)
    if op == 28:  # andi.
        return "%s = %s & 0x%04Xu; cr0_rc(c, %s);" % (R(a), R(d), uimm, R(a))
    if op == 29:  # andis.
        return "%s = %s & 0x%08Xu; cr0_rc(c, %s);" % (R(a), R(d), uimm << 16, R(a))

    # --- rotates ---
    if op in (20, 21, 23):
        sh, mb, me = b, cc, (w >> 1) & 31
        m = mask(mb, me)
        if op == 21:  # rlwinm
            rot = "rotl32(%s, %d)" % (R(d), sh) if sh else R(d)
            s = "%s = %s & 0x%08Xu;" % (R(a), rot, m)
        elif op == 20:  # rlwimi
            s = "%s = (rotl32(%s, %d) & 0x%08Xu) | (%s & 0x%08Xu);" % (R(a), R(d), sh, m, R(a), (~m) & 0xFFFFFFFF)
        else:  # rlwnm
            s = "%s = rotl32(%s, %s & 31) & 0x%08Xu;" % (R(a), R(d), R(b), m)
        return s + rc(w, R(a))

    # --- load / store D-form ---
    ld = {32: ("ld32", ""), 34: ("ld8", ""), 40: ("ld16", ""), 42: ("ld16", "(uint32_t)(int32_t)(int16_t)")}
    if op in ld or op in (33, 35, 41, 43):
        upd = op in (33, 35, 41, 43)
        fn, cast = ld[op - 1 if upd else op]
        if upd:
            return "{ uint32_t ea = %s + 0x%08Xu; %s = %s%s(ea); %s = ea; }" % (
                R(a), simm & 0xFFFFFFFF, R(d), cast, fn, R(a))
        return "%s = %s%s(%s + 0x%08Xu);" % (R(d), cast, fn, ra0(a), simm & 0xFFFFFFFF)
    st = {36: "st32", 38: "st8", 44: "st16"}
    if op in st or op in (37, 39, 45):
        upd = op in (37, 39, 45)
        fn = st[op - 1 if upd else op]
        if upd:
            return "{ uint32_t ea = %s + 0x%08Xu; %s(ea, %s); %s = ea; }" % (R(a), simm & 0xFFFFFFFF, fn, R(d), R(a))
        return "%s(%s + 0x%08Xu, %s);" % (fn, ra0(a), simm & 0xFFFFFFFF, R(d))
    if op == 46:  # lmw
        return "{ uint32_t ea = %s + 0x%08Xu; %s }" % (
            ra0(a), simm & 0xFFFFFFFF, " ".join("%s = ld32(ea + %d);" % (R(r), 4 * (r - d)) for r in range(d, 32)))
    if op == 47:  # stmw
        return "{ uint32_t ea = %s + 0x%08Xu; %s }" % (
            ra0(a), simm & 0xFFFFFFFF, " ".join("st32(ea + %d, %s);" % (4 * (r - d), R(r)) for r in range(d, 32)))

    # --- FP load/store D-form ---
    if op in (48, 49, 50, 51):  # lfs lfsu lfd lfdu
        upd = op in (49, 51)
        single = op in (48, 49)
        base = R(a) if upd else ra0(a)
        load = ("{ double v = ldf32(ea); %s = v; %s = v; }" % (F0(d), F1(d))) if single else ("%s = ldf64(ea);" % F0(d))
        return "{ uint32_t ea = %s + 0x%08Xu; %s%s }" % (base, simm & 0xFFFFFFFF, load, " %s = ea;" % R(a) if upd else "")
    if op in (52, 53, 54, 55):  # stfs stfsu stfd stfdu
        upd = op in (53, 55)
        fn = "stf32" if op in (52, 53) else "stf64"
        base = R(a) if upd else ra0(a)
        return "{ uint32_t ea = %s + 0x%08Xu; %s(ea, %s);%s }" % (
            base, simm & 0xFFFFFFFF, fn, F0(d), " %s = ea;" % R(a) if upd else "")
    if op in (56, 57, 60, 61):  # psq_l psq_lu psq_st psq_stu
        off = sext(w & 0xFFF, 12) & 0xFFFFFFFF
        pw, pi = (w >> 15) & 1, (w >> 12) & 7
        upd = op in (57, 61)
        base = R(a) if upd else ra0(a)
        fn = "psq_load" if op in (56, 57) else "psq_store"
        return "{ uint32_t ea = %s + 0x%08Xu; %s(c, %d, ea, %d, %d);%s }" % (
            base, off, fn, d, pw, pi, " %s = ea;" % R(a) if upd else "")

    if op == 31:
        return translate31(addr, w, ctx, d, a, b)
    if op == 59:
        return translate59(w, d, a, b, cc)
    if op == 63:
        return translate63(w, d, a, b, cc)
    if op == 4:
        return translate4(w, d, a, b, cc)
    if op == 3:  # twi
        to = d
        return trap_cond(to, R(a), "0x%08Xu" % (simm & 0xFFFFFFFF), addr)
    if op == 17:  # sc
        return "ppc_unimplemented(c, 0x%08Xu, 0x%08Xu); /* sc */" % (addr, w)
    raise Unhandled("op=%d" % op)


def trap_cond(to, x, y, addr):
    conds = []
    if to & 16: conds.append("(int32_t)%s < (int32_t)%s" % (x, y))
    if to & 8: conds.append("(int32_t)%s > (int32_t)%s" % (x, y))
    if to & 4: conds.append("%s == %s" % (x, y))
    if to & 2: conds.append("%s < %s" % (x, y))
    if to & 1: conds.append("%s > %s" % (x, y))
    if to == 31:
        return "ppc_trap(c, 0x%08Xu);" % addr
    if not conds:
        return ";"
    return "if (%s) ppc_trap(c, 0x%08Xu);" % (" || ".join(conds), addr)


def translate31(addr, w, ctx, d, a, b):
    xo = (w >> 1) & 0x3FF
    xo9 = (w >> 1) & 0x1FF
    oe = (w >> 10) & 1
    rD, rA, rB = R(d), R(a), R(b)

    def ov_add(x, y, res):
        return " { uint8_t o = (uint8_t)((~(%s ^ %s) & (%s ^ %s)) >> 31); c->xer_ov = o; c->xer_so |= o; }" % (x, y, x, res)

    # XO-form arithmetic (9-bit XO with OE)
    if xo9 == 266:  # add
        s = "%s = %s + %s;" % (rD, rA, rB)
        if oe:
            s = "{ uint32_t x = %s, y = %s, r = x + y; %s = r;%s }" % (rA, rB, rD, ov_add("x", "y", "r"))
        return s + rc(w, rD)
    if xo9 == 40:  # subf
        s = "%s = %s - %s;" % (rD, rB, rA)
        if oe:
            s = "{ uint32_t x = ~%s, y = %s, r = y - %s; %s = r;%s }" % (rA, rB, rA, rD, ov_add("x", "y", "r"))
        return s + rc(w, rD)
    if xo9 == 10:  # addc
        return "{ uint64_t t = (uint64_t)%s + %s; %s = (uint32_t)t; c->xer_ca = (uint8_t)(t >> 32); }%s" % (rA, rB, rD, rc(w, rD))
    if xo9 == 138:  # adde
        return "{ uint64_t t = (uint64_t)%s + %s + c->xer_ca; %s = (uint32_t)t; c->xer_ca = (uint8_t)(t >> 32); }%s" % (rA, rB, rD, rc(w, rD))
    if xo9 == 8:  # subfc
        return "{ uint64_t t = (uint64_t)(uint32_t)~%s + %s + 1; %s = (uint32_t)t; c->xer_ca = (uint8_t)(t >> 32); }%s" % (rA, rB, rD, rc(w, rD))
    if xo9 == 136:  # subfe
        return "{ uint64_t t = (uint64_t)(uint32_t)~%s + %s + c->xer_ca; %s = (uint32_t)t; c->xer_ca = (uint8_t)(t >> 32); }%s" % (rA, rB, rD, rc(w, rD))
    if xo9 == 234:  # addme
        return "{ uint64_t t = (uint64_t)%s + c->xer_ca + 0xFFFFFFFFu; %s = (uint32_t)t; c->xer_ca = (uint8_t)(t >> 32); }%s" % (rA, rD, rc(w, rD))
    if xo9 == 202:  # addze
        return "{ uint64_t t = (uint64_t)%s + c->xer_ca; %s = (uint32_t)t; c->xer_ca = (uint8_t)(t >> 32); }%s" % (rA, rD, rc(w, rD))
    if xo9 == 232:  # subfme
        return "{ uint64_t t = (uint64_t)(uint32_t)~%s + c->xer_ca + 0xFFFFFFFFu; %s = (uint32_t)t; c->xer_ca = (uint8_t)(t >> 32); }%s" % (rA, rD, rc(w, rD))
    if xo9 == 200:  # subfze
        return "{ uint64_t t = (uint64_t)(uint32_t)~%s + c->xer_ca; %s = (uint32_t)t; c->xer_ca = (uint8_t)(t >> 32); }%s" % (rA, rD, rc(w, rD))
    if xo9 == 104:  # neg
        s = "%s = (uint32_t)-(int32_t)%s;" % (rD, rA) if not oe else \
            "{ uint8_t o = %s == 0x80000000u; %s = 0u - %s; c->xer_ov = o; c->xer_so |= o; }" % (rA, rD, rA)
        return s + rc(w, rD)
    if xo9 == 235:  # mullw
        s = "%s = (uint32_t)((int32_t)%s * (int32_t)%s);" % (rD, rA, rB)
        if oe:
            s = "{ int64_t t = (int64_t)(int32_t)%s * (int32_t)%s; %s = (uint32_t)t; uint8_t o = t != (int32_t)t; c->xer_ov = o; c->xer_so |= o; }" % (rA, rB, rD)
        return s + rc(w, rD)
    if xo9 == 75:  # mulhw
        return "%s = (uint32_t)(((int64_t)(int32_t)%s * (int32_t)%s) >> 32);%s" % (rD, rA, rB, rc(w, rD))
    if xo9 == 11:  # mulhwu
        return "%s = (uint32_t)(((uint64_t)%s * %s) >> 32);%s" % (rD, rA, rB, rc(w, rD))
    if xo9 == 491:  # divw
        s = "%s = ppc_divw(%s, %s);" % (rD, rA, rB)
        if oe:
            s = "{ uint8_t o = %s == 0 || (%s == 0x80000000u && %s == 0xFFFFFFFFu); %s c->xer_ov = o; c->xer_so |= o; }" % (rB, rA, rB, s)
        return s + rc(w, rD)
    if xo9 == 459:  # divwu
        s = "%s = ppc_divwu(%s, %s);" % (rD, rA, rB)
        if oe:
            s = "{ uint8_t o = %s == 0; %s c->xer_ov = o; c->xer_so |= o; }" % (rB, s)
        return s + rc(w, rD)

    # X-form logical: rS = d
    rS = rD
    logic = {28: "%s & %s", 444: "%s | %s", 316: "%s ^ %s", 60: "%s & ~%s", 412: "%s | ~%s",
             476: "~(%s & %s)", 124: "~(%s | %s)", 284: "~(%s ^ %s)"}
    if xo in logic:
        if xo == 444 and d == b:  # mr
            return "%s = %s;%s" % (rA, rS, rc(w, rA))
        return "%s = %s;%s" % (rA, logic[xo] % (rS, rB), rc(w, rA))
    if xo == 24:  # slw
        return "%s = (%s & 0x20) ? 0 : %s << (%s & 31);%s" % (rA, rB, rS, rB, rc(w, rA))
    if xo == 536:  # srw
        return "%s = (%s & 0x20) ? 0 : %s >> (%s & 31);%s" % (rA, rB, rS, rB, rc(w, rA))
    if xo == 792:  # sraw
        return ("{ uint32_t n = %s & 0x3F; int32_t s = (int32_t)%s; if (n > 31) { %s = (uint32_t)(s >> 31); c->xer_ca = s < 0; }"
                " else { %s = (uint32_t)(s >> n); c->xer_ca = s < 0 && n && ((uint32_t)s << (32 - n)) != 0; } }%s") % (
            rB, rS, rA, rA, rc(w, rA))
    if xo == 824:  # srawi
        sh = b
        if sh == 0:
            return "%s = %s; c->xer_ca = 0;%s" % (rA, rS, rc(w, rA))
        return "{ int32_t s = (int32_t)%s; c->xer_ca = s < 0 && (s & 0x%08X); %s = (uint32_t)(s >> %d); }%s" % (
            rS, (1 << sh) - 1, rA, sh, rc(w, rA))
    if xo == 26:  # cntlzw
        return "%s = %s ? (uint32_t)__builtin_clz(%s) : 32;%s" % (rA, rS, rS, rc(w, rA))
    if xo == 954:  # extsb
        return "%s = (uint32_t)(int32_t)(int8_t)%s;%s" % (rA, rS, rc(w, rA))
    if xo == 922:  # extsh
        return "%s = (uint32_t)(int32_t)(int16_t)%s;%s" % (rA, rS, rc(w, rA))

    # compares
    if xo == 0:
        return "cr_set_s(c, %d, (int32_t)%s, (int32_t)%s);" % (d >> 2, rA, rB)
    if xo == 32:
        return "cr_set_u(c, %d, %s, %s);" % (d >> 2, rA, rB)

    # indexed loads/stores
    ea = "%s + %s" % (ra0(a), rB)
    ldx = {23: ("ld32", ""), 87: ("ld8", ""), 279: ("ld16", ""), 343: ("ld16", "(uint32_t)(int32_t)(int16_t)")}
    ldux = {55: 23, 119: 87, 311: 279, 375: 343}
    if xo in ldx:
        fn, cast = ldx[xo]
        return "%s = %s%s(%s);" % (rD, cast, fn, ea)
    if xo in ldux:
        fn, cast = ldx[ldux[xo]]
        return "{ uint32_t ea = %s + %s; %s = %s%s(ea); %s = ea; }" % (rA, rB, rD, cast, fn, rA)
    stx = {151: "st32", 215: "st8", 407: "st16"}
    stux = {183: 151, 247: 215, 439: 407}
    if xo in stx:
        return "%s(%s, %s);" % (stx[xo], ea, rS)
    if xo in stux:
        return "{ uint32_t ea = %s + %s; %s(ea, %s); %s = ea; }" % (rA, rB, stx[stux[xo]], rS, rA)
    if xo == 534:  # lwbrx
        return "%s = __builtin_bswap32(ld32(%s));" % (rD, ea)
    if xo == 790:  # lhbrx
        return "%s = __builtin_bswap16(ld16(%s));" % (rD, ea)
    if xo == 662:  # stwbrx
        return "st32(%s, __builtin_bswap32(%s));" % (ea, rS)
    if xo == 918:  # sthbrx
        return "st16(%s, __builtin_bswap16((uint16_t)%s));" % (ea, rS)
    if xo == 20:  # lwarx
        return "%s = ppc_lwarx(c, %s);" % (rD, ea)
    if xo == 150:  # stwcx.
        return "ppc_stwcx(c, %s, %s);" % (ea, rS)
    if xo == 597:  # lswi
        n = b or 32
        stmts, r = [], d - 1
        for i in range(n):
            if i % 4 == 0:
                r = (r + 1) & 31
                stmts.append("%s = 0;" % R(r))
            stmts.append("%s |= (uint32_t)ld8(ea + %d) << %d;" % (R(r), i, 24 - 8 * (i % 4)))
        return "{ uint32_t ea = %s; %s }" % (ra0(a), " ".join(stmts))
    if xo == 725:  # stswi
        n = b or 32
        stmts, r = [], d - 1
        for i in range(n):
            if i % 4 == 0:
                r = (r + 1) & 31
            stmts.append("st8(ea + %d, (uint8_t)(%s >> %d));" % (i, R(r), 24 - 8 * (i % 4)))
        return "{ uint32_t ea = %s; %s }" % (ra0(a), " ".join(stmts))

    # FP indexed
    fpx = {535: ("s", 0), 567: ("s", 1), 599: ("d", 0), 631: ("d", 1)}
    if xo in fpx:
        kind, upd = fpx[xo]
        base = "%s + %s" % (rA if upd else ra0(a), rB)
        load = ("{ double v = ldf32(ea); %s = v; %s = v; }" % (F0(d), F1(d))) if kind == "s" else "%s = ldf64(ea);" % F0(d)
        return "{ uint32_t ea = %s; %s%s }" % (base, load, " %s = ea;" % rA if upd else "")
    fsx = {663: ("stf32", 0), 695: ("stf32", 1), 727: ("stf64", 0), 759: ("stf64", 1)}
    if xo in fsx:
        fn, upd = fsx[xo]
        base = "%s + %s" % (rA if upd else ra0(a), rB)
        return "{ uint32_t ea = %s; %s(ea, %s);%s }" % (base, fn, F0(d), " %s = ea;" % rA if upd else "")
    if xo == 983:  # stfiwx
        return "st32(%s, (uint32_t)f64_as_u64(%s));" % (ea, F0(d))

    # cache ops are no-ops except dcbz; sync/lwsync/eieio are real barriers: the host (ARM) reorders
    # memory accesses across cores more aggressively than Espresso, and games publish data to other
    # cores with them
    if xo == 1014:  # dcbz
        return "ppc_dcbz(%s);" % ea
    if xo in (598, 854):  # sync (incl. lwsync), eieio
        return "__atomic_thread_fence(__ATOMIC_SEQ_CST);"
    if xo in (86, 54, 278, 246, 470, 982):  # dcbf dcbst dcbt dcbtst dcbi icbi
        return ";"

    # SPR
    spr = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5)
    if xo == 339:  # mfspr
        if spr == 8: return "%s = c->lr;" % rD
        if spr == 9: return "%s = c->ctr;" % rD
        if spr == 1: return "%s = ppc_mfxer(c);" % rD
        if 912 <= spr <= 919: return "%s = c->gqr[%d];" % (rD, spr - 912)
        if 896 <= spr <= 903: return "%s = c->gqr[%d];" % (rD, spr - 896)
        if spr in (268, 269): return "%s = (uint32_t)(ppc_timebase() >> %d);" % (rD, 0 if spr == 268 else 32)
        return "%s = 0; /* mfspr %d */" % (rD, spr)
    if xo == 467:  # mtspr
        if spr == 8: return "c->lr = %s;" % rS
        if spr == 9: return "c->ctr = %s;" % rS
        if spr == 1: return "ppc_mtxer(c, %s);" % rS
        if 912 <= spr <= 919: return "c->gqr[%d] = %s;" % (spr - 912, rS)
        if 896 <= spr <= 903: return "c->gqr[%d] = %s;" % (spr - 896, rS)
        return "; /* mtspr %d */" % spr
    if xo == 371:  # mftb
        return "%s = (uint32_t)(ppc_timebase() >> %d);" % (rD, 0 if spr == 268 else 32)
    if xo == 19:  # mfcr
        return "%s = ppc_mfcr(c);" % rD
    if xo == 144:  # mtcrf
        return "ppc_mtcrf(c, 0x%02X, %s);" % ((w >> 12) & 0xFF, rS)
    if xo == 512:  # mcrxr
        f = d >> 2
        return "c->cr[%d] = c->xer_so; c->cr[%d] = c->xer_ov; c->cr[%d] = c->xer_ca; c->cr[%d] = 0; c->xer_so = c->xer_ov = c->xer_ca = 0;" % (
            4 * f, 4 * f + 1, 4 * f + 2, 4 * f + 3)
    if xo == 4:  # tw
        return trap_cond(d, rA, rB, addr)
    raise Unhandled("op31 xo=%d" % xo)


def fp_rc(w):
    return " c->cr[4] = c->cr[5] = c->cr[6] = c->cr[7] = 0;" if w & 1 else ""


def translate59(w, d, a, b, cc):
    xo = (w >> 1) & 31
    fa, fb, fc = F0(a), F0(b), F0(cc)
    ex = {
        18: "%s / %s" % (fa, fb),
        20: "%s - %s" % (fa, fb),
        21: "%s + %s" % (fa, fb),
        25: "%s * round25(%s)" % (fa, fc),
        28: "%s * round25(%s) - %s" % (fa, fc, fb),
        29: "%s * round25(%s) + %s" % (fa, fc, fb),
        30: "-(%s * round25(%s) - %s)" % (fa, fc, fb),
        31: "-(%s * round25(%s) + %s)" % (fa, fc, fb),
        24: "ppc_fres(%s)" % fb,
    }
    if xo not in ex:
        raise Unhandled("op59 xo=%d" % xo)
    return "{ double v = to_single(%s); %s = v; %s = v; }%s" % (ex[xo], F0(d), F1(d), fp_rc(w))


def translate63(w, d, a, b, cc):
    xo5 = (w >> 1) & 31
    fa, fb, fc = F0(a), F0(b), F0(cc)
    if xo5 in (18, 20, 21, 22, 23, 25, 26, 28, 29, 30, 31):
        ex = {
            18: "%s / %s" % (fa, fb),
            20: "%s - %s" % (fa, fb),
            21: "%s + %s" % (fa, fb),
            22: "sqrt(%s)" % fb,
            23: "ppc_fsel(%s, %s, %s)" % (fa, fb, fc),
            25: "%s * %s" % (fa, fc),
            26: "ppc_frsqrte(%s)" % fb,
            28: "fma(%s, %s, -%s)" % (fa, fc, fb),
            29: "fma(%s, %s, %s)" % (fa, fc, fb),
            30: "-fma(%s, %s, -%s)" % (fa, fc, fb),
            31: "-fma(%s, %s, %s)" % (fa, fc, fb),
        }[xo5]
        return "%s = %s;%s" % (F0(d), ex, fp_rc(w))
    xo = (w >> 1) & 0x3FF
    if xo in (0, 32):  # fcmpu / fcmpo
        return "cr_set_f(c, %d, %s, %s);" % (d >> 2, fa, fb)
    if xo == 12:  # frsp
        return "{ double v = to_single(%s); %s = v; %s = v; }%s" % (fb, F0(d), F1(d), fp_rc(w))
    if xo == 15:
        return "%s = u64_as_f64(ppc_fctiwz(%s));%s" % (F0(d), fb, fp_rc(w))
    if xo == 14:
        return "%s = u64_as_f64(ppc_fctiw(c, %s));%s" % (F0(d), fb, fp_rc(w))
    if xo == 72:  # fmr
        return "%s = %s;%s" % (F0(d), fb, fp_rc(w))
    if xo == 40:
        return "%s = -%s;%s" % (F0(d), fb, fp_rc(w))
    if xo == 264:
        return "%s = fabs(%s);%s" % (F0(d), fb, fp_rc(w))
    if xo == 136:
        return "%s = -fabs(%s);%s" % (F0(d), fb, fp_rc(w))
    if xo == 583:  # mffs
        return "%s = u64_as_f64(0xFFF8000000000000ull | c->fpscr);" % F0(d)
    if xo == 711:  # mtfsf
        fm = (w >> 17) & 0xFF
        m = 0
        for i in range(8):
            if fm & (0x80 >> i):
                m |= 0xF0000000 >> (4 * i)
        return "c->fpscr = (c->fpscr & 0x%08Xu) | ((uint32_t)f64_as_u64(%s) & 0x%08Xu);" % ((~m) & 0xFFFFFFFF, fb, m)
    if xo == 134:  # mtfsfi
        crf = d >> 2
        imm = (w >> 12) & 0xF
        sh = 28 - 4 * crf
        return "c->fpscr = (c->fpscr & ~0x%08Xu) | 0x%08Xu;" % (0xF << sh, imm << sh)
    if xo == 38:  # mtfsb1
        return "c->fpscr |= 0x%08Xu;" % (0x80000000 >> d)
    if xo == 70:  # mtfsb0
        return "c->fpscr &= ~0x%08Xu;" % (0x80000000 >> d)
    if xo == 64:  # mcrfs
        fd, fs = d >> 2, a >> 2
        sh = 28 - 4 * fs
        return "{ uint32_t v = (c->fpscr >> %d) & 0xF; c->cr[%d] = v >> 3; c->cr[%d] = (v >> 2) & 1; c->cr[%d] = (v >> 1) & 1; c->cr[%d] = v & 1; }" % (
            sh, 4 * fd, 4 * fd + 1, 4 * fd + 2, 4 * fd + 3)
    raise Unhandled("op63 xo=%d" % xo)


def translate4(w, d, a, b, cc):
    xo5 = (w >> 1) & 31
    A0, A1, B0, B1, C0, C1 = F0(a), F1(a), F0(b), F1(b), F0(cc), F1(cc)

    def pair(e0, e1):
        return "{ double v0 = to_single(%s), v1 = to_single(%s); %s = v0; %s = v1; }%s" % (e0, e1, F0(d), F1(d), fp_rc(w))

    if xo5 in (10, 11, 12, 13, 14, 15, 18, 20, 21, 23, 24, 25, 26, 28, 29, 30, 31):
        if xo5 == 10: return "{ double v0 = to_single(%s + %s), v1 = %s; %s = v0; %s = v1; }" % (A0, B1, C1, F0(d), F1(d))
        if xo5 == 11: return "{ double v0 = %s, v1 = to_single(%s + %s); %s = v0; %s = v1; }" % (C0, A0, B1, F0(d), F1(d))
        if xo5 == 12: return pair("%s * round25(%s)" % (A0, C0), "%s * round25(%s)" % (A1, C0))
        if xo5 == 13: return pair("%s * round25(%s)" % (A0, C1), "%s * round25(%s)" % (A1, C1))
        if xo5 == 14: return pair("%s * round25(%s) + %s" % (A0, C0, B0), "%s * round25(%s) + %s" % (A1, C0, B1))
        if xo5 == 15: return pair("%s * round25(%s) + %s" % (A0, C1, B0), "%s * round25(%s) + %s" % (A1, C1, B1))
        if xo5 == 18: return pair("%s / %s" % (A0, B0), "%s / %s" % (A1, B1))
        if xo5 == 20: return pair("%s - %s" % (A0, B0), "%s - %s" % (A1, B1))
        if xo5 == 21: return pair("%s + %s" % (A0, B0), "%s + %s" % (A1, B1))
        if xo5 == 23:
            return "{ double v0 = ppc_fsel(%s, %s, %s), v1 = ppc_fsel(%s, %s, %s); %s = v0; %s = v1; }" % (
                A0, B0, C0, A1, B1, C1, F0(d), F1(d))
        if xo5 == 24: return pair("ppc_fres(%s)" % B0, "ppc_fres(%s)" % B1)
        if xo5 == 25: return pair("%s * round25(%s)" % (A0, C0), "%s * round25(%s)" % (A1, C1))
        if xo5 == 26: return pair("ppc_frsqrte(%s)" % B0, "ppc_frsqrte(%s)" % B1)
        if xo5 == 28: return pair("%s * round25(%s) - %s" % (A0, C0, B0), "%s * round25(%s) - %s" % (A1, C1, B1))
        if xo5 == 29: return pair("%s * round25(%s) + %s" % (A0, C0, B0), "%s * round25(%s) + %s" % (A1, C1, B1))
        if xo5 == 30: return pair("-(%s * round25(%s) - %s)" % (A0, C0, B0), "-(%s * round25(%s) - %s)" % (A1, C1, B1))
        if xo5 == 31: return pair("-(%s * round25(%s) + %s)" % (A0, C0, B0), "-(%s * round25(%s) + %s)" % (A1, C1, B1))
    if xo5 in (6, 7):  # psq_lx psq_stx psq_lux psq_stux
        xo6 = (w >> 1) & 0x3F
        upd = xo6 in (38, 39)
        pw, pi = (w >> 10) & 1, (w >> 7) & 7
        base = "%s + %s" % (R(a) if upd else ra0(a), R(b))
        fn = "psq_load" if xo5 == 6 else "psq_store"
        return "{ uint32_t ea = %s; %s(c, %d, ea, %d, %d);%s }" % (base, fn, d, pw, pi, " %s = ea;" % R(a) if upd else "")
    xo = (w >> 1) & 0x3FF
    if xo in (0, 32):
        return "cr_set_f(c, %d, %s, %s);" % (d >> 2, A0, B0)
    if xo in (64, 96):
        return "cr_set_f(c, %d, %s, %s);" % (d >> 2, A1, B1)
    un = {40: "-%s", 72: "%s", 136: "-fabs(%s)", 264: "fabs(%s)"}
    if xo in un:
        e = un[xo]
        return "{ double v0 = %s, v1 = %s; %s = v0; %s = v1; }%s" % (e % B0, e % B1, F0(d), F1(d), fp_rc(w))
    merge = {528: (A0, B0), 560: (A0, B1), 592: (A1, B0), 624: (A1, B1)}
    if xo in merge:
        e0, e1 = merge[xo]
        # ps_merge rounds ps0 of the double source to single on real hardware only
        # when the source holds a non-single value; Cemu copies directly.
        return "{ double v0 = %s, v1 = %s; %s = v0; %s = v1; }%s" % (e0, e1, F0(d), F1(d), fp_rc(w))
    if xo == 1014:  # dcbz_l
        return "ppc_dcbz(%s + %s);" % (ra0(a), R(b))
    raise Unhandled("op4 xo=%d" % xo)
