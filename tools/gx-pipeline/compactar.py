#!/usr/bin/env python3
# tools/gx-pipeline: compacta las unidades generadas output_N.cpp para que el compilador las
# optimice mejor (ver compact_function). Escribe tools/gx-pipeline/compacto/output_N.cpp; solo
# rehace las que han cambiado. Lo llaman Reconstruir-Clang.ps1 y Reconstruir-O2Pipe.ps1.
#   python tools/gx-pipeline/compactar.py [--max-segundos N]
import os, re, sys

RE_FUNC = re.compile(r'^(?:extern "C" )?void \w+\(CPUContext& ctx\) \{$')
RE_CASE = re.compile(r'^\s*case 0x([0-9A-Fa-f]+): goto loc_([0-9A-Fa-f]+);\s*$')
RE_LABEL = re.compile(r'^loc_([0-9A-Fa-f]+): ;\s*$')
RE_GOTO = re.compile(r'goto loc_([0-9A-Fa-f]+);')
RE_PC = re.compile(r'ctx\.pc (?:=|!=|==) 0x([0-9A-Fa-f]+)')
RE_LR = re.compile(r'ctx\.lr = 0x([0-9A-Fa-f]+)')
TRIGGERS = ('goto', 'return', '(ctx)', 'process_pending_callbacks', 'read_timebase', 'read_dec', 'write_dec', 'dec_', 'interpret', 'inst_count', 'switch', 'trace_call', 'handle_syscall')


# Direcciones por las que se puede entrar en una funcion desde FUERA de ella: casos de las tablas
# de despacho (export/dispatch_*.cpp: destinos de tablas de saltos por bctr, vueltas, etc.) y
# cualquier ctx.pc = / ctx.lr = 0x... de todo el codigo. Antes solo se miraba la propia funcion y
# se quitaban, p. ej., los destinos de las tablas de saltos: al entrar por ellos se ejecutaba el
# interprete de reserva (con instrucciones sin implementar), y los monstruos se quedaban
# caminando sin avanzar (1 nov). Ver [Interprete] en el registro.
GLOBAL_KEEP = frozenset()
RE_DISP = re.compile(r'case 0x([0-9A-Fa-f]+): \w+\(ctx\); break;')


def _scan_refs(path):
    txt = open(path, encoding='utf-8', errors='replace').read()
    out = set(int(m.group(1), 16) for m in RE_DISP.finditer(txt))
    out.update(int(m.group(1), 16) for m in RE_PC.finditer(txt))
    out.update(int(m.group(1), 16) for m in RE_LR.finditer(txt))
    return out


def _init_keep(keep):
    global GLOBAL_KEEP
    GLOBAL_KEEP = keep


def compact_function(lines):
    """lines: cuerpo completo de una funcion (incluida la cabecera y la llave final)."""
    # localizar el switch de entrada: 'if (ctx.pc != 0x...) {' seguido de 'switch (ctx.pc) {'
    sw_start = sw_end = -1
    for i in range(1, min(len(lines), 6)):
        if lines[i].strip().startswith('if (ctx.pc != 0x') and lines[i].rstrip().endswith('{') and i + 1 < len(lines) and lines[i + 1].strip() == 'switch (ctx.pc) {':
            sw_start = i + 2
            j = sw_start
            while j < len(lines) and lines[j].strip() != '}':
                j += 1
            sw_end = j   # linea '}' que cierra el switch
            break
    keep = set(GLOBAL_KEEP)
    for i, l in enumerate(lines):
        if sw_start <= i < sw_end:
            continue
        for m in RE_GOTO.finditer(l):
            keep.add(int(m.group(1), 16))
        for m in RE_PC.finditer(l):
            keep.add(int(m.group(1), 16))
        for m in RE_LR.finditer(l):
            keep.add(int(m.group(1), 16))
    out = []
    removed = 0
    for i, l in enumerate(lines):
        if sw_start <= i < sw_end:
            m = RE_CASE.match(l)
            if m and int(m.group(1), 16) not in keep:
                removed += 1
                continue
        out.append(l)
    lines = out
    # etiquetas sin uso
    used = set(keep)
    for l in lines:
        m = RE_CASE.match(l)
        if m:
            used.add(int(m.group(2), 16))
    out = []
    for l in lines:
        m = RE_LABEL.match(l)
        if m and int(m.group(1), 16) not in used:
            continue
        out.append(l)
    lines = out
    # juntar ++ctx.inst_count
    out = []
    pending = 0
    body_start = 1
    if sw_end >= 0:
        for k in range(1, len(lines)):
            if lines[k].rstrip() == '    }':
                body_start = k + 1   # tras el '}' que cierra el if del switch de entrada
                break
    def flush():
        nonlocal pending
        if pending:
            out.append('    ctx.inst_count += %d;' % pending if pending > 1 else '    ++ctx.inst_count;')
            pending = 0
    for i, l in enumerate(lines):
        if i < body_start or i == len(lines) - 1:
            if i == len(lines) - 1:
                flush()
            out.append(l)
            continue
        s = l.rstrip('\r\n')
        if s == '    ++ctx.inst_count;':
            pending += 1
            continue
        if RE_LABEL.match(s):
            flush()
            out.append(l)
            continue
        top = s.startswith('    ') and not s.startswith('     ')
        if not top:
            out.append(l)   # dentro de un bloque: el bloque ya provoco el volcado
            continue
        st = s.strip()
        if st.startswith('//'):
            out.append(l)
            continue
        if RE_LABEL.match(st) or st.endswith('{') or st == '}' or any(t in st for t in TRIGGERS):
            flush()
        out.append(l)
    return out, removed



# Tras compactar: el codigo recompilado nunca guarda el puntero al contexto. Se marca asi
# (__restrict en la definicion y llamadas sin captura, ver port_ne en cpu_context.h) para que el
# compilador pueda tener los registros del juego en registros del host entre accesos a memoria.
# Ademas se quita el aviso de depuracion "Spinning at PC" de cada bucle.
RE_DEF = re.compile(r'^((?:extern "C" )?void \w+\((?:nwii::runtime::)?CPUContext)& ctx\) \{$')
RE_LDECL = re.compile(r'^(\s*extern "C" void \w+\((?:nwii::runtime::)?CPUContext ?&)( ?ctx)?\);\s*$')
RE_SPIN = re.compile(r'^\s*if \(\(ctx\.inst_count % 10000000\) == 0\) \{ std::cout << "Spinning at PC.*$')
RE_NE = re.compile(r'(?<![\w:&])((?:nwii::runtime::)?(?:micro_interpret|CacheLoopBatchCount|TryAcceleratedTHP)|port_mem::Copy|port_mem::Fill|port_thp::Decode)\(ctx\b')


# Llamadas indirectas (bctrl: funciones virtuales, punteros a funcion). El generador las hace
# volviendo al despachador (run_game): se deshace toda la pila del host y luego cada funcion de
# la cadena se vuelve a entrar desde run_game al ir volviendo (setjmp + comprobacion de eventos +
# despacho por cada nivel). Ahora se llama al destino directamente (port_dispatch, copia de
# dispatch_recompiled) igual que una llamada directa; si no es una funcion conocida, como antes.
RE_ICALL = re.compile(r'^(\s*)if \(ctx\.ctr\) \{ ctx\.pc = ctx\.ctr; return; \} else (nwii::runtime::note_null_call\(0x[0-9A-Fa-f]+, ctx\.lr\);)\s*$')
RE_SAVELR = re.compile(r'^\s*ctx\.lr = 0x([0-9A-Fa-f]+); // save return address\s*$')


def harden(lines):
    out = []
    for l in lines:
        if RE_SPIN.match(l):
            continue
        mi = RE_ICALL.match(l)
        if mi:
            ret = None
            for prev in reversed(out[-4:]):
                ms = RE_SAVELR.match(prev)
                if ms:
                    ret = ms.group(1)
                    break
            if ret:
                out.append(f'{mi.group(1)}if (ctx.ctr) {{ ctx.pc = ctx.ctr; if (!(nwii::runtime::g_port_dcalls && port_dispatch(ctx, ctx.pc)) || ctx.pc != 0x{ret}) return; }} else {mi.group(2)}')
                continue
        md = RE_LDECL.match(l)
        if md:
            out.append(md.group(1) + (md.group(2) or '') + ' PORT_NOESC);')
            continue
        m = RE_DEF.match(l)
        if m:
            l = m.group(1) + '& PORT_RESTRICT ctx PORT_NOESC) {'
        else:
            if 'ctx' in l:
                l = l.replace('nwii::runtime::interpret_step(ctx)', 'nwii::runtime::port_ne_interp(ctx)')
                l = l.replace('nwii::runtime::trace_call(', 'nwii::runtime::port_ne_trace(')
                l = l.replace('nwii::runtime::handle_syscall(ctx)', 'nwii::runtime::port_ne_syscall(ctx)')
                l = RE_NE.sub(r'nwii::runtime::port_ne(&\1)(ctx', l)
        out.append(l)
    return out


# Saltos de cola por CTR (bctr sin enlace: envoltorios que llaman a un puntero a funcion). El
# generador vuelve al despachador; la funcion destino vuelve luego a la direccion de vuelta de
# quien nos llamo y toda la cadena se reconstruye desde run_game. Si el destino esta fuera de esta
# funcion se llama directamente (port_dispatch) y se vuelve: quien nos llamo ve su direccion de
# vuelta y sigue. Si esta dentro (tabla de saltos) o no se conoce, como antes.
RE_TAIL = re.compile(r'^(\s*)ctx\.pc = ctx\.ctr; return;\s*$')
RE_ADDR = re.compile(r'^\s*// 0x([0-9A-Fa-f]{8}): ')


def tail_calls(body):
    addrs = [int(m.group(1), 16) for m in map(RE_ADDR.match, body) if m]
    if not addrs:
        return body
    lo, span = min(addrs), max(addrs) + 4 - min(addrs)
    out = []
    for l in body:
        m = RE_TAIL.match(l)
        if m:
            l = f'{m.group(1)}ctx.pc = ctx.ctr; if (nwii::runtime::g_port_dcalls && ctx.pc - 0x{lo:08X}u >= 0x{span:X}u) port_dispatch(ctx, ctx.pc); return;'
        out.append(l)
    return out


def port_dispatch_source(root):
    """port_dispatch: copia de dispatch_recompiled de export/main_output.cpp (alli es static)."""
    txt = open(os.path.join(root, 'export', 'main_output.cpp'), encoding='utf-8', errors='replace').read().replace('\r\n', '\n')
    head = 'static bool dispatch_recompiled(CPUContext& ctx, uint32_t target) {\n'
    i = txt.index(head)
    j = txt.index('\n}\n', i) + 3
    body = txt[i:j].replace(head, 'bool port_dispatch(CPUContext& ctx PORT_NOESC, uint32_t target) {\n', 1)
    names = sorted(set(re.findall(r'\b(dispatch_[0-9A-F]{4})\(ctx, target\)', body)))
    assert names, 'port_dispatch: sin dispatch_XXXX'
    decl = ''.join(f'bool {n}(CPUContext& ctx, uint32_t target);\n' for n in names)
    return ('\n// tools/gx-pipeline (compactar.py): copia de dispatch_recompiled (main_output.cpp) para las llamadas\n'
            '// indirectas del codigo compactado.\n' + decl + body)


def write_functions_h(exp, outd):
    """compacto/functions.h: las mismas declaraciones con PORT_NOESC (las unidades la incluyen antes que la de export)."""
    src = open(os.path.join(exp, 'functions.h'), encoding='utf-8', newline='').read()
    txt = src.replace('nwii::runtime::CPUContext& ctx);', 'nwii::runtime::CPUContext& ctx PORT_NOESC);')
    txt = txt.rstrip() + '\n// tools/gx-pipeline (compactar.py): despacho para llamadas indirectas, definido al final de compacto/output_1.cpp\nbool port_dispatch(nwii::runtime::CPUContext& ctx PORT_NOESC, uint32_t target);\n'
    dst = os.path.join(outd, 'functions.h')
    if not os.path.exists(dst) or open(dst, encoding='utf-8', newline='').read() != txt:
        open(dst, 'w', encoding='utf-8', newline='').write(txt)


def compact_file(src, dst):
    text = open(src, encoding='utf-8', newline='').read()
    nl = '\r\n' if '\r\n' in text else '\n'
    lines = text.split(nl)
    out = []
    i = 0
    removed = 0
    while i < len(lines):
        if RE_FUNC.match(lines[i]):
            j = i + 1
            while j < len(lines) and lines[j] != '}':
                j += 1
            body, rem = compact_function(lines[i:j + 1])
            body = tail_calls(body)
            out.extend(body)
            removed += rem
            i = j + 1
        else:
            out.append(lines[i])
            i += 1
    out = harden(out)
    if os.path.basename(dst) == 'output_1.cpp':
        out += port_dispatch_source(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))).split('\n')
    tmp = dst + '.tmp'
    open(tmp, 'w', encoding='utf-8', newline='').write(nl.join(out))
    os.replace(tmp, dst)
    return removed


def _job(args):
    src, dst = args
    return os.path.basename(dst), compact_file(src, dst)


def main():
    import argparse, time
    ap = argparse.ArgumentParser()
    ap.add_argument('--max-segundos', type=float, default=0)
    ap.add_argument('--hilos', type=int, default=0)
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(os.path.dirname(here))
    exp = os.path.join(root, 'export')
    outd = os.path.join(here, 'compacto')
    os.makedirs(outd, exist_ok=True)
    me = os.path.getmtime(os.path.abspath(__file__))
    write_functions_h(exp, outd)
    jobs = []
    for name in sorted(os.listdir(exp)):
        if not re.fullmatch(r'output_\d+\.cpp', name):
            continue
        src = os.path.join(here, name) if os.path.exists(os.path.join(here, name)) else os.path.join(exp, name)
        dst = os.path.join(outd, name)
        if os.path.exists(dst) and os.path.getmtime(dst) > max(os.path.getmtime(src), me):
            continue
        jobs.append((src, dst))
    print(f'[compactar] {len(jobs)} unidades por compactar', flush=True)
    keep = set()
    if jobs:
        srcs = [os.path.join(exp, n) for n in sorted(os.listdir(exp)) if re.fullmatch(r'(output_\d+|dispatch_[0-9A-Fa-f]+|main_output)\.cpp', n)]
        srcs += [os.path.join(here, n) for n in ('output_1.cpp', 'output_170.cpp') if os.path.exists(os.path.join(here, n))]
        from concurrent.futures import ProcessPoolExecutor
        with ProcessPoolExecutor(max_workers=a.hilos or None) as ex:
            for r in ex.map(_scan_refs, srcs):
                keep |= r
        print(f'[compactar] {len(keep)} direcciones de entrada desde fuera', flush=True)
    keep = frozenset(keep)
    _init_keep(keep)
    t0 = time.time()
    if a.max_segundos:   # en serie, parando a tiempo (para entornos con limite de tiempo)
        for j in jobs:
            if time.time() - t0 > a.max_segundos:
                print('[compactar] parado por tiempo; vuelve a ejecutarlo', flush=True)
                return 2
            n, r = _job(j)
            print(f'  {n}: {r} entradas quitadas', flush=True)
    elif jobs:
        from concurrent.futures import ProcessPoolExecutor
        with ProcessPoolExecutor(max_workers=a.hilos or None, initializer=_init_keep, initargs=(keep,)) as ex:
            for n, r in ex.map(_job, jobs):
                print(f'  {n}: {r} entradas quitadas', flush=True)
    print(f'[compactar] listo en {time.time() - t0:.0f} s', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
