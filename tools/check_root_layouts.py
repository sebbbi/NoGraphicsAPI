#!/usr/bin/env python3
"""Check every GPU_ROOT field against C++, Slang SPIR-V reflection and native Metal layout (macOS)."""

import argparse
import concurrent.futures
import json
from pathlib import Path
import re
import subprocess


def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def structs(text):
    text = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
    for match in re.finditer(r"\bstruct\s+(?:ROOT_ALIGN\s+)?(\w+)\s*\{", text):
        start = end = match.end()
        depth = 1
        while depth:
            depth += (text[end] == "{") - (text[end] == "}")
            end += 1
        yield match[1], text[start:end - 1]


def fields(body):
    result = []
    for declaration in body.split(";"):
        declaration = declaration.split("=", 1)[0].strip()
        if not declaration:
            continue
        match = re.fullmatch(r"([\w:]+\s*\*?)\s+(\w+)(\[\w+\])?", declaration)
        if not match:
            raise RuntimeError(f"Unsupported root declaration: {declaration}")
        result.append((match[1].strip(), match[2], bool(match[3])))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", nargs="+", type=Path, help="Shader and shared-header directories to scan")
    parser.add_argument("--include", action="append", default=[], type=Path)
    parser.add_argument("--slang", required=True, type=Path)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--output", required=True, type=Path, help="Scratch directory for probes and layout report")
    parser.add_argument("--jobs", default=8, type=int)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    roots = set()
    declarations = {}
    for directory in args.directories:
        for path in sorted(directory.rglob("*.slang")):
            roots.update(re.findall(r"\bGPU_ROOT\(\s*(\w+)", path.read_text()))
        for path in sorted(directory.rglob("*.h")):
            for name, body in structs(path.read_text()):
                declarations[name] = (path.resolve(), body)
    if not roots or roots - declarations.keys():
        raise RuntimeError(f"Missing root declarations: {sorted(roots - declarations.keys())}")
    includes = [part for path in args.include for part in ("-I", str(path.resolve()))]
    roots = sorted(roots)
    paths = {}

    def visit(name, prefix=""):
        for typename, field, array in fields(declarations[name][1]):
            member = prefix + field
            yield member
            if array:
                yield member + "[0]"
            if typename in declarations:
                yield from visit(typename, member + ("[0]." if array else "."))

    layouts = {root: {} for root in roots}

    def check(root):
        source = ["#include <stdio.h>", "#include <stddef.h>", f'#include "{declarations[root][0]}"', "int main() {"]
        paths[root] = list(visit(root))
        source.append(f'printf("{root} @ %zu %zu\\n", sizeof({root}), alignof({root}));')
        for field in paths[root]:
            source.append(f'printf("{root} {field} %zu %zu\\n", offsetof({root}, {field}), sizeof((({root}*)0)->{field}));')
        source.append("}")
        cpp = args.output / (root + ".cpp")
        cpp.write_text("\n".join(source) + "\n")
        executable = args.output / root
        run([args.cxx, "-std=c++20", *includes, cpp, "-o", executable])
        for line in run([executable.resolve()]).splitlines():
            _, field, offset, size = line.split()
            layouts[root][field] = [int(offset), int(size)]
        probe = args.output / (root + ".slang")
        probe.write_text(f'#include "{declarations[root][0]}"\n'
                         f'[[vk::binding(0)]] ConstantBuffer<{root}> root : register(b0);\n'
                         f'[[vk::binding(3)]] RWStructuredBuffer<{root}> result : register(u3);\n'
                         '[numthreads(1, 1, 1)] void computeMain() { result[0] = root; }\n')
        common = [args.slang.resolve(), probe, *includes, "-entry", "computeMain", "-stage", "compute",
                  "-fvk-use-c-layout", "-matrix-layout-row-major"]
        reflection = probe.with_suffix(".json")
        run(common + ["-target", "spirv", "-profile", "spirv_1_5", "-reflection-json", reflection,
                      "-o", probe.with_suffix(".spv")])
        parameters = json.loads(reflection.read_text())["parameters"]
        reflected = next(parameter for parameter in parameters if parameter["name"] == "root")["type"]["elementType"]

        def check_spirv(typename, prefix="", base=0):
            for field in typename["fields"]:
                member = prefix + field["name"]
                binding = field["binding"]
                expected = [base + binding["offset"], binding["size"]]
                if layouts[root][member] != expected:
                    raise RuntimeError(f"{root}.{member}: C++ {layouts[root][member]} != SPIR-V {expected}")
                child = field["type"]
                if child["kind"] == "array":
                    child = child["elementType"]
                    member += "[0]"
                if child["kind"] == "struct":
                    check_spirv(child, member + ".", expected[0])

        check_spirv(reflected)
        metal = probe.with_suffix(".metal")
        run(common + ["-target", "metal", "-DNOGRAPHICSAPI_METAL", "-o", metal])
        text = metal.read_text()
        native_root = re.search(r"(\w+)\s+constant\s*\*\s*\w+\s*\[\[buffer\(0\)\]\]", text)[1]
        native_structs = dict(structs(text))

        def native_path(field):
            typename = native_root
            members = []
            for part in field.split("."):
                name = part.removesuffix("[0]")
                declaration = next(line for line in native_structs[typename].split(";")
                                   if re.search(r"\b" + name + r"_\d+\s*$", line))
                native_name = re.search(r"\b(" + name + r"_\d+)\s*$", declaration)[1]
                members.append(native_name + ("[0]" if part.endswith("[0]") else ""))
                typename = declaration.strip().split()[0]
            return ".".join(members)

        assertions = [f'static_assert(sizeof({native_root}) == {layouts[root]["@"][0]}, "{root} size");']
        for field in paths[root]:
            native = native_path(field)
            offset, size = layouts[root][field]
            if "[" not in field:
                assertions.append(f'static_assert(__builtin_offsetof({native_root}, {native}) == {offset}, "{root}.{field} offset");')
            assertions.append(f'static_assert(sizeof((({native_root} constant*)0)->{native}) == {size}, "{root}.{field} size/stride");')
        metal.write_text(text + "\n" + "\n".join(assertions) + "\n")
        run(["xcrun", "-sdk", "macosx", "metal", "-std=metal4.0", "-target", "air64-apple-macosx26.0",
             "-c", metal, "-o", probe.with_suffix(".air")])
        return root

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        for root in executor.map(check, roots):
            print(f"Checked {root}: {len(paths[root])} fields/array strides", flush=True)
    (args.output / "layouts.json").write_text(json.dumps(layouts, indent=2) + "\n")
    print(f"Validated {len(roots)} roots and {sum(map(len, paths.values()))} fields/array strides against C++, SPIR-V and Metal.")


if __name__ == "__main__":
    main()
