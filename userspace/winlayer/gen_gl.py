#!/usr/bin/env python3
"""Writes gl_thunks.c: an entry point for each OpenGL function, called the
Windows way, that calls Mesa's (the Unix way).

    gen_gl.py /usr/include/GL/gl.h /usr/include/GL/glext.h > gl_thunks.c

Each function gets
    static ret (*p_glName)(args);                       Mesa's, set by gl_resolve()
    static ret WINAPI t_glName(args) { return p_glName(...); }
and a row in gl_funcs[] (name, thunk, &pointer) for wglGetProcAddress and
for opengl32.dll's exports.  Functions that take callbacks (the debug
output ones) are left to opengl32.c, which wraps the callback too."""
import re
import sys

SKIP = {
    "glDebugMessageCallback", "glDebugMessageCallbackARB", "glDebugMessageCallbackKHR",
    "glDebugMessageCallbackAMD", "glGetVkProcAddrNV",
}


def prototypes(path):
    text = open(path).read()
    # one declaration may span lines; join until the semicolon
    for m in re.finditer(r"GLAPI\s+([^;]*?)\s*(?:GLAPIENTRY|APIENTRY)\s+(gl\w+)\s*\(([^;]*?)\)\s*;", text, re.S):
        ret, name, args = (" ".join(x.split()) for x in m.groups())
        yield ret, name, args


def split_args(args):
    if args in ("", "void"):
        return []
    out, depth, cur = [], 0, ""
    for c in args:
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        if c == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += c
    out.append(cur.strip())
    return out


TYPE_WORDS = {"void", "const", "unsigned", "signed", "int", "char", "float", "double", "short", "long", "struct"}


def arg_name(decl, i):
    # "const GLfloat *v" -> v; "GLenum" (no name) -> one of ours
    words = re.findall(r"\w+", decl)
    if len(words) >= 2:
        last = words[-1]
        if not last.startswith("GL") and last not in TYPE_WORDS and not decl.rstrip().endswith("*"):
            return decl, last
    name = "a%d" % i
    return decl + " " + name, name


def main():
    seen = set()
    funcs = []
    ncore = 0
    for k, path in enumerate(sys.argv[1:]):
        for ret, name, args in prototypes(path):
            if name in seen or name in SKIP:
                continue
            seen.add(name)
            funcs.append((ret, name, split_args(args)))
        if k == 0:
            ncore = len(funcs)      # gl.h's: what opengl32.dll itself exports
    out = sys.stdout
    out.write("/* written by gen_gl.py from Mesa's GL headers: do not edit */\n")
    out.write("#include <GL/gl.h>\n#include <GL/glext.h>\n")
    out.write("#include \"win.h\"\n\n")
    out.write("typedef struct { const char *name; void *thunk; void **mesa; } gl_func_t;\n\n")
    for ret, name, args in funcs:
        decls, names = [], []
        for i, a in enumerate(args):
            d, n = arg_name(a, i)
            decls.append(d)
            names.append(n)
        params = ", ".join(decls) if decls else "void"
        call = ", ".join(names)
        out.write("static %s (*p_%s)(%s);\n" % (ret, name, params))
        body = ("return " if ret != "void" else "") + "p_%s(%s);" % (name, call)
        out.write("static %s WINAPI t_%s(%s) { %s }\n" % (ret, name, params, body))
    out.write("\nconst gl_func_t gl_funcs[] = {\n")
    for ret, name, args in funcs:
        out.write("    { \"%s\", (void *)t_%s, (void **)&p_%s },\n" % (name, name, name))
    out.write("};\nconst int gl_nfuncs = %d;\nconst int gl_ncore = %d;\n" % (len(funcs), ncore))


main()
